# Copyright © 2024 Intel Corporation
# SPDX-License-Identifier: MIT

import argparse
import sys
from math import pi

a = 'a'
b = 'b'

lower_fsign = [
    # This matches the behavior of the old optimization in brw_fs_nir.cpp, but
    # it has some problems.
    #
    # The fmul version passes Vulkan float_controls2 CTS a little bit by
    # luck. The use of fne means that the false path (i.e., fsign(X) == 0) is
    # only taken when X is zero. For OpenCL, this path should also be taken
    # when when X is NaN. This can be handled by using 'fabs(X) > 0', but this
    # fails float_controls2 CTS when the other multiplication operand is NaN.
    #
    # This optimization is additionally problematic when fsign(X) is zero and
    # the other multiplication operand is Inf. This will result in 0, but it
    # should result in NaN. This does not seem to be tested by the CTS.
    #
    # NOTE: fcsel opcodes are currently limited to float32 in NIR.
    (('fmul@32', ('fsign(is_used_once)', a), b), ('fcsel',          a    , ('ixor', ('iand', a, 0x80000000), b), ('iand', a, 0x80000000))),
    (('fmul@16', ('fsign(is_used_once)', a), b), ('bcsel', ('fneu', a, 0), ('ixor', ('iand', a, 0x8000    ), b), ('iand', a, 0x8000    ))),

    # This is 99.99% strictly correct for OpenCL. It will provide correctly
    # signed zero for ±0 inputs, and it will provide zero for NaN inputs. The
    # only slight deviation is that it can provide -0 for some NaN inputs.
    (('fsign@32', a), ('fcsel_gt',          ('fabs', a) , ('ior', ('iand', a, 0x80000000), 0x3f800000), ('iand', a, 0x80000000))),
    (('fsign@16', a), ('bcsel', ('!flt', 0, ('fabs', a)), ('ior', ('iand', a, 0x8000    ), 0x3c00    ), ('iand', a, 0x8000    ))),
]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-p', '--import-path', required=True)
    args = parser.parse_args()
    sys.path.insert(0, args.import_path)
    run()


def run():
    import nir_algebraic  # pylint: disable=import-error

    print('#include "brw_nir.h"')

    print(nir_algebraic.AlgebraicPass("brw_nir_lower_fsign", lower_fsign).render())


if __name__ == '__main__':
    main()
