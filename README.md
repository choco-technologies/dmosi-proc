# dmosi-proc

Implementation of the Process interface from [DMOSI](https://github.com/choco-technologies/dmosi) (DMOD OS Interface).

## Overview

`dmosi-proc` is a C static library that provides an implementation of the process management interface defined by the DMOSI specification. It integrates with the [DMOD](https://github.com/choco-technologies/dmod) framework and is intended for use in modular embedded or OS-like systems.

## Features

- **Process creation and destruction** – create named processes associated with a module, with optional parent process linking
- **Process lifecycle management** – kill a process (terminating all its threads) or wait for it to terminate
- **Process properties** – get and set process name, ID, UID, module name, working directory, state, and exit status
- **Process lookup** – find a running process by name or by process ID
- **Thread integration** – automatically manages threads associated with a process during kill and destroy operations

## Building

This project uses CMake (≥ 3.10) and fetches its dependencies automatically via `FetchContent`.

```sh
cmake -B build -DDMOSI_PROC_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

To build without tests:

```sh
cmake -B build
cmake --build build
```

## Dependencies

- [dmod](https://github.com/choco-technologies/dmod) – DMOD core framework
- [dmosi](https://github.com/choco-technologies/dmosi) – DMOD OS Interface definitions

Both are fetched automatically during the CMake configure step.

## License

See [LICENSE](LICENSE).
