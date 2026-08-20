use strict;
use warnings;
# Deferred warnings, and cancellation, when a transformation runs on an offload
# backend's worker thread.
#
# Both need an op that warns from inside its broadcast loop, and no shipping op
# does, so this builds one - the same trick as 01-pptest.t.  It also needs a perl
# carrying the multicore_offload hook and a backend to install in it, so almost
# everywhere this skips.
use ExtUtils::MakeMaker::Config;
use Test::More $Config{usedl}
    ? ()
    : (skip_all => 'No dynaload; double-blib static build too difficult');
use File::Spec;
use File::Basename;
use File::Path;
use IPC::Cmd qw(run);
use Cwd;

BEGIN {
  plan skip_all => 'Coro::Multicore not installed'
    unless eval { require Coro; require Coro::Multicore; 1 };
  plan skip_all => 'this perl has no multicore_offload hook'
    unless Coro::Multicore::_offload_supported();
}

my %FILES = (
    'Makefile.PL' => <<'EOF',
use strict;
use warnings;
use ExtUtils::MakeMaker;
use PDL::Core::Dev;
my @pack = (["offl.pd", qw(OfflTests PDL::OfflTests), '', 1]);
sub MY::postamble { pdlpp_postamble(@pack) }
WriteMakefile(pdlpp_stdargs(@pack));
EOF

    'offl.pd' => <<'EOF',
pp_def('offl_warnrow',
       Pars => 'a(n); [o]b()',
       GenericTypes => ['D'],
       OtherPars => 'PDL_Indx work;',
       Code => '
         double sum = 0;
         PDL_Indx reps;
         for (reps = 0; reps < $COMP(work); reps++)
           loop(n) %{ sum += $a(); %}
         $b() = sum;
         PDL->pdl_warn("offl_warnrow: summed %.0f", sum);
       ',
      );

pp_def('offl_barfrow',
       Pars => 'a(n); [o]b()',
       GenericTypes => ['D'],
       Code => '
         double sum = 0;
         loop(n) %{ sum += $a(); %}
         $b() = sum;
         PDL->pdl_barf("offl_barfrow: refusing");
       ',
      );

pp_done;
EOF

    't/all.t' => <<'EOF',
use strict;
use warnings;
use Time::HiRes qw(time);
use Test::More;
use Coro;
use Coro::AnyEvent;
use Coro::Multicore;
use PDL::LiteF;
use PDL::OfflTests;

Coro::Multicore::enable_offload (1);
alarm 300;

my $ROWS = 8;
# over the offload size threshold (1 M-element), and enough rows to split
my $x = sequence(262144, $ROWS);
$x->sum;

# NOT local: a coro has its own stack, so a localised handler would not be in
# effect inside the async blocks below.
my @w;
$SIG{__WARN__} = sub { push @w, $_[0] };
sub reported { my $n = 0; $n += () = /offl_warnrow: summed/g for @w; $n }

for my $threads (1, 4) {
  @w = ();
  PDL::set_autopthread_targ($threads);
  PDL::set_autopthread_size(1);
  $x->offl_warnrow(1);
  is reported(), $ROWS,
    "every warning from an offloaded transformation is reported ($threads pthread(s))";
  cmp_ok PDL::get_autopthread_actual(), $threads > 1 ? '>' : '==', $threads > 1 ? 1 : 0,
    "... and it did fan out as asked ($threads)";
}

# A barf has no interpreter to raise from either: it comes back as the error the
# transformation returns, whether a fan-out collected it or the worker itself did.
for my $threads (1, 4) {
  PDL::set_autopthread_targ($threads);
  my $ok = eval { $x->offl_barfrow; 1 };
  like $@, qr/offl_barfrow: refusing/,
    "a barf from an offloaded transformation is raised in the caller ($threads)";
}

# Cancelled part way through: the loop stops, the caller gets the exception it was
# sent, and the warnings the pthreads had buffered go with the frame.
{
  PDL::set_autopthread_targ(4);
  @w = ();
  my $work = 2000;                    # enough that a row takes a good fraction of a second
  my $full = time; $x->offl_warnrow($work); $full = time - $full;
  @w = ();

  my $err;
  my $victim = async { eval { $x->offl_warnrow($work); 1 } or $err = $@ };
  Coro::AnyEvent::sleep 0.05;
  $victim->throw ("stop\n");
  eval { $victim->join };

  like $err, qr/stop/, 'the exception aimed at the thread is what it gets';
  cmp_ok $full, '>', 0.3, 'the uninterrupted run is long enough for this to mean something';
  is reported(), 0, 'and the warnings buffered by its pthreads went with it';
}

PDL::set_autopthread_targ(1);
done_testing;
EOF
);

in_dir(sub {
    hash2files(File::Spec->curdir, \%FILES);
    local $ENV{PERL5LIB} = join $Config{path_sep}, @INC;
    run_ok(qq{"$^X" Makefile.PL});
    run_ok(qq{"$Config{make}" test});
});

done_testing;

sub run_ok {
    my ($cmd) = @_;
    my $res = run(command => $cmd, buffer => \my $buffer);
    ok($res, $cmd) or diag $buffer;
}

sub hash2files {
    my ($prefix, $hashref) = @_;
    while (my ($file, $text) = each %$hashref) {
        $file = File::Spec->catfile(File::Spec->curdir, $prefix, split m{\/}, $file);
        mkpath dirname($file);
        open my $fh, '>', $file or die "Can't create $file: $!";
        print $fh $text;
        close $fh;
    }
}

sub in_dir {
    my ($code, $dir) = @_;
    $dir ||= File::Spec->catdir(File::Spec->curdir, './.offloadtest/sub');
    mkpath $dir;
    my $orig_dir = getcwd();
    chdir $dir or die "Can't chdir to $dir: $!";
    my $ok = eval { $code->(); 1 };
    my $err = $@;
    chdir $orig_dir or die "Can't chdir to $orig_dir: $!";
    die $err unless $ok;
}
