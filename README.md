# obmc-console

## To Build

To build this project, run the following shell commands:

    meson setup build
    meson compile -C build

To test:

    dbus-run-session meson test -C build

## To Run Server

Running the server requires a serial port (e.g. /dev/ttyS0):

    touch obmc-console.conf
    ./obmc-console-server --config obmc-console.conf ttyS0

## To Connect Client

To connect to the server, simply run the client:

    ./obmc-console-client

To disconnect the client, use the standard `~.` combination.

## Underlying design

This shows how the host UART connection is abstracted within the BMC as a Unix
domain socket.

                +---------------------------------------------------------------------------------------------+
                |                                                                                             |
                |       obmc-console-client       unix domain socket         obmc-console-server              |
                |                                                                                             |
                |     +----------------------+                           +------------------------+           |
                |     |   client.2200.conf   |  +---------------------+  | server.ttyVUART0.conf  |           |
            +---+--+  +----------------------+  |                     |  +------------------------+  +--------+-------+
    Network    | 2200 +-->                      +->+ @obmc-console.host0 +<-+                        <--+ /dev/ttyVUART0 |   UARTs
            +---+--+  | console-id = "host0" |  |                     |  |  console-id = "host0"  |  +--------+-------+
                |     |                      |  +---------------------+  |                        |           |
                |     +----------------------+                           +------------------------+           |
                |                                                                                             |
                |                                                                                             |
                |                                                                                             |
                +---------------------------------------------------------------------------------------------+

This supports multiple independent consoles. The `console-id` is a unique
portion for the unix domain socket created by the obmc-console-server instance.
The server needs to know this because it needs to know what to name the pipe;
the client needs to know it as it needs to form the abstract socket name to
which to connect.

### Input delivery and backpressure

The server's main console descriptor is nonblocking. Client input is copied as
a complete block into a bounded 64 KiB queue and the main event loop drains the
queue whenever the descriptor reports `POLLOUT`. Positive short writes advance
only by the accepted count; `EINTR` retries the same suffix; and `EAGAIN` leaves
the suffix queued.

```text
console client input --> bounded TX queue --> POLLOUT --> upstream TTY
                              |
target TTY POLLIN ------------+---- processed first on every poll iteration
                              |
                        socket/TTY/DBus RX consumers
```

The event loop never waits for TX writability. It continues draining target RX,
D-Bus, and console sockets while the opposite direction is backpressured. This
is required for simultaneous RAS/firmware output and host-to-target commands.

If the queue cannot admit a complete input block, that producer retains the
block and temporarily removes only its `POLLIN` event. Physical-TTY progress
notifies paused producers, which retry the retained block before accepting new
input. This propagates bounded backpressure to socket and local-TTY writers
without blocking the shared event loop or closing a healthy connection.

Zero progress, hangup, and other terminal write failures remain visible
delivery errors. No path consumes later input after an incomplete block,
because doing so would turn backpressure into silent command truncation.

## Mux Support

In some hardware designs, multiple UARTS may be available behind a Mux. Please
reference
[docs/mux-support.md](https://github.com/openbmc/obmc-console/blob/master/docs/mux-support.md)
in that case.

## Sample Development Setup

For developing obmc-console, we can use pseudo terminals (pty's) in Linux.

The socat command will output names of 2 pty's, one of which is the master and
the other one is the slave. The master pty can be used to emulate a UART.

    $ socat -d -d pty,raw,echo=0,link=pty1 pty,raw,echo=0,link=pty2

    $ obmc-console-server --console-id dev $(realpath pty2)

    $ obmc-console-client -i dev

    # this message should appear for the client
    $ echo "hi" > pty1
