# Build notes

## Precision

`MS110D_PRECISION` is `double` or `single`. It is a PUBLIC compile definition on the library target, and the public types live in an inline namespace keyed on it (`v_double` or `v_single`). A program built for one precision therefore fails to link against a library built for the other, instead of silently mismatching. Do not hand-define `MS110D_SINGLE_PRECISION`; take the library through its CMake target.

## Floating point flags

`-ffp-contract=off` is on for every target we build: fused multiply-add rounds once instead of twice, which changes results, and we compare against the C# reference. The flag is PRIVATE to our targets, so public headers must not contain inline floating-point maths; it would be compiled under the consumer's flags. Put maths in `src/`.

The core is also built with `-fno-exceptions -fno-rtti`. Public headers must still compile in a consumer that has them on.
