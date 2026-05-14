/*
 *  virtuoso_adbc.h
 *
 *  Private header for the Virtuoso ADBC driver.
 *  Phase 1 skeleton -- the public ABI is the vendored adbc.h.
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  This project is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; only version 2 of the License, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *  General Public License for more details.
 */

#ifndef VIRTUOSO_ADBC_H
#define VIRTUOSO_ADBC_H

#include "adbc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Driver identity. Bumped per release.
 */
#define VIRT_ADBC_DRIVER_NAME    "ADBC Driver for Virtuoso"
#define VIRT_ADBC_DRIVER_VERSION "0.0.1-dev"

/*
 *  Phase 1 stub state. Later phases will replace these with the real
 *  VirtAdbcDatabase / VirtAdbcConnection / VirtAdbcStatement structures
 *  sketched in plan.md.
 */
struct virt_adbc_stub {
    int initialised;
};

#ifdef __cplusplus
}
#endif

#endif /* VIRTUOSO_ADBC_H */
