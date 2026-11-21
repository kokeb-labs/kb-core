# components/lib/utils/fuzz/corpus/

One folder per fuzz target with its seed inputs. Inputs are binary files (often `.bin`); they are part of the
repository and replayed by the host tests, so never delete them to make a build pass.
