use strict;
use warnings;
# Would PDL's asynchronous entry point work with Future::AsyncAwait?
#
# What FAA does with a *foreign* awaitable is a short, documented sequence - see
# Future::AsyncAwait::Awaitable - and it does not care who made the object or
# whether an offload happened.  This drives that sequence by hand over the handle
# make_physical_async returns, on all three of its paths: already resolved,
# pending, and failed.
#
# What this does not cover: FAA's own keyword machinery, and delivery through an
# event loop that is not Coro's.  Both are outside the handle.
use Test::More;

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

plan tests => 11;

alarm 300;

my $N = 20_000_000;

# On `await $f`, FAA takes AWAIT_GET at once if AWAIT_IS_READY.  Otherwise it
# clones the awaited object to make the future the async sub itself returns,
# registers AWAIT_ON_READY on the awaited one, and suspends; on that callback it
# takes AWAIT_GET and resolves the clone with AWAIT_DONE or AWAIT_FAIL.
sub mini_await {
   my ($f) = @_;

   return ("ready", [ $f->AWAIT_GET ]) if $f->AWAIT_IS_READY;

   my $ret = $f->AWAIT_CLONE;

   $f->AWAIT_ON_READY (sub {
      my @v = eval { $f->AWAIT_GET };
      $@ ? $ret->AWAIT_FAIL ($@) : $ret->AWAIT_DONE (@v);
   });

   ("suspended", $ret)
}

sub pending_sum {
   my $a = PDL->zeroes ($N)->random;
   $a->flowing;                       # dataflow: the operation is set up, not run
   ($a, $a->sumover)                  # the parent must stay in scope
}

# --- the ready path: no backend, so the handle is resolved before it is seen ----
{
   my ($a, $b) = pending_sum ();
   my $h = $b->make_physical_async;
   my ($state, $got) = mini_await ($h);

   is $state, "ready", "with no backend the awaited handle is already resolved";
   isa_ok $got->[0], "PDL", "AWAIT_GET yields the ndarray";
   cmp_ok abs ($got->[0]->sclr / $N - 0.5), '<', 0.01, "and the right value";
}

Coro::Multicore::enable_offload (1);

# --- the suspend path: a pending handle, resolved from the event loop ----------
{
   my ($state, $clone, $got, $pending_at_first);

   async {
      my ($a, $b) = pending_sum ();
      my $h = $b->make_physical_async;

      $pending_at_first = !$h->AWAIT_IS_READY;
      ($state, $clone) = mini_await ($h);

      return if $state ne "suspended";

      until ($clone->AWAIT_IS_READY) { Coro::AnyEvent::sleep 0.002 }
      $got = [ $clone->AWAIT_GET ];
   }->join;

   ok $pending_at_first, "the handle comes back pending, as an await needs";
   is $state, "suspended", "so the awaiting side takes the clone-and-register path";
   isa_ok $clone, "Coro::Multicore::Offload::Awaitable", "AWAIT_CLONE yields";
   ok $clone->AWAIT_IS_READY, "the clone resolves when the offload finishes";
   isa_ok $got->[0], "PDL", "and carries the ndarray";
   cmp_ok abs ($got->[0]->sclr / $N - 0.5), '<', 0.01, "with the right value";
}

# --- the failure path: a cancelled transformation must fail the clone ----------
{
   my ($clone, $err);

   async {
      my ($a, $b) = pending_sum ();
      my $h = $b->make_physical_async;
      (my $state, $clone) = mini_await ($h);

      return if $state ne "suspended";

      $h->cancel;
      until ($clone->AWAIT_IS_READY) { Coro::AnyEvent::sleep 0.002 }
      $err = eval { $clone->AWAIT_GET; undef } || $@;
   }->join;

   ok $clone->AWAIT_IS_READY, "a cancelled transformation still resolves the clone";
   isa_ok $err, "PerlMulticore::Cancelled", "as a failure, which AWAIT_GET raises";
}
