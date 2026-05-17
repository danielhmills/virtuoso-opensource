/*
 *  program_info.c
 *
 *  Program metadata required by libutil when the ADBC driver is loaded
 *  as a standalone module through the Arrow ADBC driver manager.
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  Licensed under the GNU GPL v2; see COPYING in the project root.
 */

#include "libutil.h"

struct pgm_info program_info = {
    "VIRTADBC",
    NULL,
    NULL,
    0,
    NULL
};
