A project whose only script raises an error in its first `OnUpdate` (the failing instance is then disabled
and the game keeps running). `RuntimeOption_fail-on-script-errors` checks that a
`--frames` run with `--fail-on-script-errors` reports it (exit code 7) instead of exiting 0.
