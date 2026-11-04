#include "toy_uapi.h"
_Static_assert(_IOC_SIZE(TOY_GET_INFO)==56,"GET_INFO ioctl size");
_Static_assert(_IOC_SIZE(TOY_GET_VF)==72,"GET_VF ioctl size");
_Static_assert(_IOC_SIZE(TOY_RESET_VF)==80,"RESET ioctl size");
_Static_assert(__builtin_offsetof(struct toy_vf_info,lease_token)==48,"lease offset");
_Static_assert(__builtin_offsetof(struct toy_event,session)==24,"session offset");
