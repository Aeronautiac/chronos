# Fails if sim or util code calls a libm function whose rounding isn't
# guaranteed to be the same on every platform: those break determinism
# across environments. Use util/det_math.hpp (det::exp, det::sin, ...)
# instead, or add what's missing there. + - * /, sqrt, floor, ceil, abs,
# min/max and friends are exactly rounded, so they're fine.
#
# Run as: cmake -DSCAN_DIRS="dir;dir" -P no_libm_in_sim.cmake

set(BANNED "exp|exp2|expm1|log|log2|log10|log1p|pow|sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|asinh|acosh|atanh|hypot|cbrt|fmod|remainder|erf|erfc|tgamma|lgamma|sinf|cosf|expf|powf")

set(found "")
foreach(dir ${SCAN_DIRS})
  file(GLOB_RECURSE files "${dir}/*.cpp" "${dir}/*.hpp")
  foreach(f ${files})
    file(STRINGS "${f}" lines REGEX "std::(${BANNED})[ \t]*\\(")
    foreach(line ${lines})
      string(STRIP "${line}" line)
      list(APPEND found "  ${f}: ${line}")
    endforeach()
  endforeach()
endforeach()

if(found)
  list(JOIN found "\n" report)
  message(FATAL_ERROR "libm calls in the sim (use util/det_math.hpp):\n${report}")
endif()
