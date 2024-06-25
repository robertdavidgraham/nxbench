# nxbench - next generation webserver benchmarking

**(unfinished)**

What makes modern webservers (like `nginx`) different from old webservers (like `apache`) is **scalability**,
the ability to handle at least 10,000 concurrent connections. Traditional benchmark tools, like the `ab`
program that comes with Apache, aren't very good at stressing this. By now, there are problem some
excellent tools to do this, but I can't easily find. Instead, I created this weekend project to create
my own.

The reason Apache's `ab` doesn't scale is because it's using only a pair of IP address. This means
we'll quickly run out of port numbers on the client side.

The key feature of this tool is that it supports many source and destination IP addresses. On Linux,
it's very easy to configure many temporary IP addresses on both sides. Thus, we can send requests from
10 source IP address and 10 target IP addresses and achieve 100 as many concurrent connections as
`ab`. Each request chooses a random source and destination IP address.

The **status** of this project is early development. I've barely got it working.

In particular, it seems I need to investigate client-side issues tuning the network stack.
First of all, by default, a process isn't allowed to have 10,000 open file handles at once.


