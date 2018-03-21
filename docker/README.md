avcut - docker build container
==============================


this container allows to build avcut
without install dependencies on the host

Build the container
-------------------

Build `docker build -t avcut .`

Compile
-------------------
Change to an empty directory (e.g. /home/your_user/avcut
Checkout `git clone https://github.com/anyc/avcut.git .`
Compile `docker run --rm -v /home/your_user/avcut/:/build avcut`
Binary is placed inside your directory if command finished