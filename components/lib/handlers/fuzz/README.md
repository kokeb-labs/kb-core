# components/lib/handlers/fuzz/

`fuzz_handlers_channel`: any sequence of caller posts, owner takes, deferred and immediate completions and caller
waits (with a zero timeout, so callers give up) on a `RequestChannel` follows a model of the slot protocol: a reply
always belongs to its own request, no slot is lost or shared, an abandoned request is never completed to anyone,
and `Busy` comes exactly when every slot is in use. `corpus/fuzz_handlers_channel/` holds the seeds, replayed as a
CTest on every compiler.
