# Runs the ReXenon instruction test bench (see run.py): builds, generates, compiles, runs,
# and writes results.txt next to this script. Arguments pass through (--filter, --jobs, --no-build).
py -I "$PSScriptRoot\run.py" @args
exit $LASTEXITCODE
