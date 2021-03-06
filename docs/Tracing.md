# Event Tracing in KV-Engine

KV-Engine utilises [Phosphor](http://github.com/couchbase/phosphor) to achieve
high performance event tracing.

KV-Engine is explicitly tooled using trace macros which log timestamps and
some associated metadata (categories, names, arguments). An example of such a
trace macro is given below:

    TRACE_EVENT2("category", "name", "arg1", 12345, "arg2," 67.89);

The full selection of trace macros is documented in the
[Phosphor header file](https://github.com/couchbase/phosphor/blob/master/include/phosphor/phosphor.h).

These events are stored into a fixed size in-memory ringbuffer which can be
dumped to JSON via [`kv_trace_dump`](../engines/ep/management/kv_trace_dump),
and then viewed usng the Trace Viewer built into Google Chrome
 ([chrome://tracing](chrome://tracing)).

## Enabling Tracing

Tracing can be enabled in a basic mode through the use of the
`PHOSPHOR_TRACING_START` environment variable. An example in conjunction with
the memcached testapp is shown here:

     PHOSPHOR_TRACING_START="save-on-stop:testapp.%p.json" ./memcached_testapp --gtest_filter="*GetSetTest*"

This will run the memcached Get/Set tests and dump a file in the form
'testapp.<pid>.json' into the current directory each time the memcached daemon
exits. This json file can then be loaded into the Google chrome trace viewer
(chrome://tracing).

The environment variable accepts any string-form tracing config as described
below.

Tracing can also be controlled via the IOCTL MCBP commands - the easiest way
to do this is with the mcctl executable:

    $ ./mcctl -h localhost:11210 get trace.status
    enabled

- `get trace.status`: Returns the current tracing status, either 'enabled' or
'disabled'
- `get trace.config`: Returns the current tracing config
- `get trace.dump.begin`: Converts the last trace into a new dump and returns
the uuid of the new dump
- `get trace.dump.chunk?id=<uuid>`: Returns the next chunk from the dump of
the given uuid
- `set trace.config`: Sets the tracing config
- `set trace.start`: Starts tracing
- `set trace.stop`: Stops tracing
- `set trace.dump.clear`: Clears the the dump specified by uuid in the value

A trace can be performed via IOCTL using the following steps

    set trace.start
    <do stuff you want traced>
    set trace.stop
    get trace.dump.begin (save the returned uuid)
    get trace.dump.chunk?id=<uuid> (and repeat until you recieve an empty chunk)
    set trace.dump.clear <uuid>

The chunks must then be concatenated to assemble the full JSON dump. This can be
done trivially with mcctl and bash:

    $ ./mcctl -h localhost:11210 get trace.dump.chunk?id=<uuid> >> trace.json

## Tracing Config
There are several semi-colon (';') separated options that can be used as part of
a tracing config.

- save-on-stop: Save to a file when tracing stops, accepts %p (pid) and %d
(timestamp) placeholders for the given filename.
- buffer-mode: Accepts one of 'ring' or 'fixed' for a buffer which either
overwrites itself when full or stops tracing when full.
- buffer-size: The size of the trace buffer in bytes to be created
- enabled-categories: A comma-separated list of categories to be enabled. This
supports basic globbing '*' and '?'.
- disabled-categories: A comma-separated list of categories to be explicitly
disabled (This mask is applied after the enabled-categories mask), also supports
globbing.

Example Config:

    buffer-mode:ring;buffer-size:20000000;enabled-categories:memcached/*;disabled-categories:memcached/state_machine

This particular config would create a 20MB ring buffer, with all memcached
categories enabled except for the memcached 'state_machine' category.

## Tracing Categories

Categories are free-form strings, however a number of standard categories are
defined:

* `memcached/*` - Memcached related events.
* `ep-engine/*` - ep-engine related events.
    * `ep-engine/task` - ep-engine Task executions
* `mutex` - Mutex wait and lock events. Can be costly to record as each mutex
  `lock()` / `unlock()` pair requires 3 calls to `clock_gettime()`. Disabled
  by default.
