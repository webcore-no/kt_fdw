/*-------------------------------------------------------------------------
 *
 * Kyoto Tycoon Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2013 Cloudflare Inc
 * Copyright (c) 2017-2021 DigitalGarden AS
 *
 * This software is released under the AGNU and in part MIT
 *
 * Author: Odin Hultgren Van Der Horst <odin@digitalgarden.no>
 *
 * IDENTIFICATION
 *        kt_fdw/sql/kt_fdw.c
 *
 *-------------------------------------------------------------------------
 */

create function kt_fdw_handler()
returns fdw_handler
as 'MODULE_PATHNAME'
language c strict;

create function kt_fdw_validator(text[], oid)
returns void
as 'MODULE_PATHNAME'
language c strict;

create foreign data wrapper kt_fdw
  handler kt_fdw_handler
  validator kt_fdw_validator;
