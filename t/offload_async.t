use strict;
use warnings;
# PDL's asynchronous entry point: make_physical_async runs a pending
# transformation off the interpreter thread and hands back the offload backend's
# HANDLE instead of waiting for it.
#
# The ordinary path (an eager $b = $a->sumover, offloaded and waited for inside
# pdl__ensure_trans) is covered by t/offload.t.  This is the other one, for a
# caller that wants to hold the operation: several at once, or awaited from a
# stackless program.  It needs a perl with the multicore_offload hook and a
# backend to install in it, so almost everywhere this skips.
use Test::More;
use Time::HiRes qw(time);

BEGIN {
  plan skip_all => 'Coro::Multicore not installed'
    unless eval { require Coro; require Coro::AnyEvent; require Coro::Multicore; 1 };
  plan skip_all => 'this perl has no multicore_offload hook'
    unless Coro::Multicore::_offload_supported();
  plan skip_all => 'this PDL was built without multicore offload support'
    unless eval { require PDL::Core; PDL::Core::offload_supported() };

  $ENV{PERL_ANYEVENT_MODEL} ||= "EV";
}

use Coro;
use Coro::AnyEvent;
use Coro::Multicore;
use PDL::LiteF;

plan tests => 24;

alarm 300;

Coro::Multicore::enable_offload (1);

# Big enough to clear the eligibility threshold with room to spare, so that "still
# running when the call returned" is not a photo finish.
my $N = 20_000_000;

sub flowing_sum {
   my $a = PDL->zeroes ($N)->random;
   $a->flowing;                  # dataflow sets the transformation up, unrun
   my $b = $a->sumover;
   ok !$b->allocated, "the transformation is set up but has not run";
   ($a, $b)
}

# --- the shape, and that the interpreter really is free ---------------------
{
   my ($ticks, $value, $was_pending) = (0);

   async {
      my ($a, $b) = flowing_sum ();
      my $h = $b->make_physical_async;

      ok $h->can ('get') && $h->can ('AWAIT_IS_READY'),
         "make_physical_async returns something that implements the protocol";

      $was_pending = !$h->AWAIT_IS_READY;

      my $ticker = async { while (!$h->AWAIT_IS_READY) { $ticks++; Coro::AnyEvent::sleep 0.005 } };

      my $out = $h->get;

      isa_ok $out, "PDL", "the handle resolves to the ndarray";
      $value = $out->sclr / $N;
      $ticker->join;
   }->join;

   ok $was_pending, "the call returned before the work was over";
   cmp_ok $ticks, '>', 0, "another coro ran while it was running";
   cmp_ok abs ($value - 0.5), '<', 0.01, "and the result is right";
}

# --- what the handle buys: several ops in flight from ONE coro --------------
{
   my $slots = Coro::Multicore::_offload_free_slots ();
   my (@r, $in_flight);

   async {
      my @a = map { my $x = PDL->zeroes ($N / 4)->random; $x->flowing; $x } 1 .. 3;
      my @h = map { $_->sumover->make_physical_async } @a;

      $in_flight = $slots - Coro::Multicore::_offload_free_slots ();

      @r = map { $_->get->sclr / ($N / 4) } @h;
   }->join;

   is $in_flight, 3, "three transformations in flight at once, from one coro";
   ok !(grep { abs ($_ - 0.5) > 0.01 } @r), "all three results are right";
   is Coro::Multicore::_offload_free_slots (), $slots, "slots returned";
}

# --- nothing to offload: still a handle ------------------------------------
# An ndarray with nothing pending, or a transformation too small to be worth a
# worker, is made physical on the spot - and still comes back in a handle, so the
# caller never has to ask which happened.
{
   async {
      my $h = PDL->zeroes (10)->random->make_physical_async;

      ok $h->AWAIT_IS_READY, "an ndarray with nothing pending gives a resolved handle";
      isa_ok $h->get, "PDL", "carrying the ndarray";
   }->join;
}

# --- cancellation ----------------------------------------------------------
# The error the transformation reports lands in done (), which croaks it; the
# backend turns that into the handle's failure, so get () raises it.
{
   my ($err, $recomputed);

   async {
      my ($a, $b) = flowing_sum ();
      my $h = $b->make_physical_async;

      Coro::AnyEvent::sleep 0.005;
      $h->cancel;

      $err = eval { $h->get; undef } || $@;
      $recomputed = $b->sclr / $N;      # a cancelled result must not be handed out
   }->join;

   ok ref $err && $err->isa ("PerlMulticore::Cancelled"),
      "a cancelled transformation fails the handle, with the shared exception";
   like "$err", qr/cancelled/, "carrying the transformation's own message";
   is $err->get, undef,
      "and salvaging nothing: the outputs are part-written, not partial results";
   cmp_ok abs ($recomputed - 0.5), '<', 0.01,
      "and the next read runs it again rather than returning half an answer";
}

# --- safe_cancel: stop it without blocking the interpreter -----------------
{
   my ($ticks, $err) = (0);

   async {
      my ($a, $b) = flowing_sum ();
      my $h = $b->make_physical_async;

      Coro::AnyEvent::sleep 0.005;

      my $cleanup = $h->safe_cancel;
      my $ticker  = async { until ($h->AWAIT_IS_READY) { $ticks++; Coro::AnyEvent::sleep 0.005 } };

      $cleanup->get;
      $err = eval { $h->get; undef } || $@;
      $ticker->join;
   }->join;

   cmp_ok $ticks, '>', 0, "safe_cancel let other coros run while the work stopped";
   ok ref $err && $err->isa ("PerlMulticore::Cancelled"),
      "and the handle ends up cancelled all the same";
}

# --- the caller drops every ndarray reference while the work runs ----------
# With the handle out in the open this can happen at any moment, and destroying a
# participating ndarray would destroy the transformation with it - while the worker
# is reading it.  The job retains them for exactly this.
{
   my $value;

   async {
      my $h;
      {
         my ($a, $b) = flowing_sum ();
         $h = $b->make_physical_async;
      }
      $value = $h->get->sclr / $N;
   }->join;

   cmp_ok abs ($value - 0.5), '<', 0.01, "dropping the ndarrays mid-flight is safe";
}

# --- the handle itself is dropped while pending ----------------------------
{
   my $slots = Coro::Multicore::_offload_free_slots ();
   my $abandons = Coro::Multicore::_offload_abandons ();

   async {
      my ($a, $b) = flowing_sum ();
      my $h = $b->make_physical_async;
      undef $h;
   }->join;

   is Coro::Multicore::_offload_abandons (), $abandons + 1,
      "dropping a pending handle abandons the work";
   is Coro::Multicore::_offload_free_slots (), $slots, "and the slot comes back";
}
