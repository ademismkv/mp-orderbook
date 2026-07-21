## 2026-07-18 — second real cargo build, circular include

Second real build attempt hit a different bug: the C++ header included the `cxx`-generated header before declaring its own class, creating a circular include that broke a required type alias the generated code depends on.

Fixed by using forward declarations in the header (sufficient for the method signatures involved) and including the generated header only in the implementation files that actually need the complete type definitions.
