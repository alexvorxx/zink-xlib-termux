#!/usr/bin/env python3
#
# Copyright © 2024 Igalia S.L.
# SPDX-License-Identifier: MIT

template = """/*
 * Copyright © 2024 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "util/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

% for name, enum in enums.items():
enum PACKED ${prefix}_${name} {
% for k, v in enum.items():
   ${k} = ${v},
% endfor
};

% endfor

#ifdef __cplusplus
} /* extern C */
#endif
"""

import sys, os
sys.path.append(os.path.dirname(os.path.abspath(__file__)) + "/../../compiler/isaspec")

from mako.template import Template
from isa import ISA

def main():
    isa = ISA(sys.argv[1])
    prefix = 'isa'
    enums = {}

    for name, enum in isa.enums.items():
        name = name.replace('#', '')
        e = {}

        for k, v in enum.values.items():
            v = v.get_name().replace('.', '').replace('[', '').replace(']', '').upper()
            item = prefix.upper() + '_' + name.upper() + '_' + v
            e[item] = k

        enums[name] = e

    print(Template(template).render(prefix=prefix, enums=enums))

if __name__ == '__main__':
    main()
