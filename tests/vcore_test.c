/*
 * Copyright (c) 2011 The Regents of the University of California
 * Kevin Klues <klueska@cs.berkeley.edu>
 *
 * This file is part of Parlib.
 * 
 * Parlib is free software: you can redistribute it and/or modify
 * it under the terms of the Lesser GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Parlib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Lesser GNU General Public License for more details.
 * 
 * See COPYING.LESSER for details on the GNU Lesser General Public License.
 * See COPYING for details on the GNU General Public License.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "atomic.h"
#include "tls.h"
#include "vcore.h"

void vcore_entry()
{
  if(vcore_saved_ucontext) {
    void *cuc = vcore_saved_ucontext;
    printf("Restoring context: entry %d, num_vcores: %ld\n", vcore_id(), num_vcores());
    set_tls_desc(vcore_saved_tls_desc);
    parlib_setcontext(cuc);
    assert(0);
  }

  printf("entry %d, num_vcores: %ld\n", vcore_id(), num_vcores());

  do {
    vcore_request(1);
  } while (vcore_id() == 0);

  vcore_yield();
}

int main()
{
  vcore_lib_init();
  printf("main, max_vcores: %ld\n", max_vcores());
  __set_tls_desc(vcore_tls_descs(0), 0);
  vcore_saved_ucontext = NULL;  
  vcore_entry();
}
