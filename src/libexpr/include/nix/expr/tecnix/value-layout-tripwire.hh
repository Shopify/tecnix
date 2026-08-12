#pragma once
///@file

static_assert(
    sizeof(void *) != 8 || sizeof(Value) == 16,
    "Tecnix value labels live in the sparse label table; Value must stay at upstream's pointer-pair size");
