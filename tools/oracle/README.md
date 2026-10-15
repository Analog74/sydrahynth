# Reference oracle

Differential tests that compare this project's code with the reference plugin
(HydrasynthVST 0.0.1, x86_64 slice). Functions of the reference are executed
inside Unicorn (an emulated CPU), never natively. Nothing from the reference is
part of the build; you need your own copy of it to run these.

Setup:

    python3 -m venv /tmp/hydra-venv && /tmp/hydra-venv/bin/pip install unicorn
    export HYDRA_REFERENCE=/path/to/HydrasynthVST.x86_64   # thin x86_64 slice
    export HYDRA_FIXED_DATA=/path/to/fixed_data.bin

`fixed_data.bin` holds the reference's data segments with Mach-O chained fixups
applied (needed for its absolute jump tables). Export it from an IDA database
of the slice: for the `__got`, `__const` (DATA), `__cfstring`, `__objc_const`,
`__data`, `__thread_vars` and `__init_offsets` segments, write
`u64 start, u64 size, bytes` records.

Tests:

    python -I decode_diff.py build/dsptest            # DSP decoder, ~1.6M words
    python -I exec_diff.py build/dsptest 500 2000 1   # DSP execution: cases, budget, seed
