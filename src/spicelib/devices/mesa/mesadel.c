/**********
Copyright 1990 Regents of the University of California.  All rights reserved.
Author: 1985 S. Hwang
**********/

/*
  Imported into mesa model: 2001 Paolo Nenzi
*/

#include "ngspice/ngspice.h"
#include "mesadefs.h"
#include "ngspice/sperror.h"
#include "ngspice/suffix.h"


int
MESAdelete(GENinstance *gen_inst)
{
    NG_IGNORE(gen_inst);
    return OK;
}