# Warning and floating point flags shared by every target we build.
function(ms110d_apply_options target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror
      -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
      -Wold-style-cast -Wcast-qual -Wnon-virtual-dtor -Woverloaded-virtual
      # Fused multiply-add rounds once instead of twice, which changes floating
      # point results. We compare sample by sample against the C# reference, so
      # contraction is off everywhere, whatever the target CPU offers.
      -ffp-contract=off)
  endif()
endfunction()
