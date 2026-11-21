# tests/install_consumer/

A small CMake project per package that installs kb-core into a temporary prefix and uses it through
`find_package(kb CONFIG REQUIRED COMPONENTS <part>)`, proving the exported targets, include paths and version.
