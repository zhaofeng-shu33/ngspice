#include <config.h>

#include <devdefs.h>

#include "jfet2itf.h"
#include "jfet2ext.h"
#include "jfet2init.h"


SPICEdev JFET2info = {
    {
        "JFET2",
        "Short channel field effect transistor",

        &JFET2nSize,
        &JFET2nSize,
        JFET2names,

        &JFET2pTSize,
        JFET2pTable,

        &JFET2mPTSize,
        JFET2mPTable,
	DEV_DEFAULT
    },

    DEVparam      : JFET2param,
    DEVmodParam   : JFET2mParam,
    DEVload       : JFET2load,
    DEVsetup      : JFET2setup,
    DEVunsetup    : JFET2unsetup,
    DEVpzSetup    : JFET2setup,
    DEVtemperature: JFET2temp,
    DEVtrunc      : JFET2trunc,
    DEVfindBranch : NULL,
    DEVacLoad     : JFET2acLoad,
    DEVaccept     : NULL,
    DEVdestroy    : JFET2destroy,
    DEVmodDelete  : JFET2mDelete,
    DEVdelete     : JFET2delete,
    DEVsetic      : JFET2getic,
    DEVask        : JFET2ask,
    DEVmodAsk     : JFET2mAsk,
    DEVpzLoad     : NULL, /* AN_pz */
    DEVconvTest   : NULL,
    DEVsenSetup   : NULL,
    DEVsenLoad    : NULL,
    DEVsenUpdate  : NULL,
    DEVsenAcLoad  : NULL,
    DEVsenPrint   : NULL,
    DEVsenTrunc   : NULL,
    DEVdisto      : NULL, /* AN_disto */
    DEVnoise      : JFET2noise,
                    
    DEVinstSize   : &JFET2iSize,
    DEVmodSize    : &JFET2mSize

};


SPICEdev *
get_jfet2_info(void)
{
    return &JFET2info;
}
