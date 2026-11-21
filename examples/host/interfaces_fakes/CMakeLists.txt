# examples/host/interfaces_fakes: see README.md. Built with KB_BUILD_EXAMPLES and run as the ctest test example_interfaces_fakes.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_example(interfaces_fakes REQUIRES kb_interfaces kb_interfaces_host LIBS kb_interfaces_host kb::interfaces)
