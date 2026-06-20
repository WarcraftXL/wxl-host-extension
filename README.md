# wxl-host-extension

A WarcraftXL module for the asset host that keeps recently served files in memory and reads ahead of
the client.

When the client asks the host for a file, the host can hand back a copy it already holds instead of
fetching it again, and it can quietly pull in the files that one is likely to need next. The point is
simply to make the client wait less on disk.

Part of [WarcraftXL](../../README.md). Released under the GNU General Public License v3.0.
