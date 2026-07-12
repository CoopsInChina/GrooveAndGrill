#pragma once

// AUTO-GENERATED from data/meat_temps.json — do not hand-edit.
// Regenerate with: python3 scripts/gen_meat_temps.py

typedef struct {
    const char *label;
    int         target_c;
} meat_level_t;

typedef struct {
    const char         *name;
    const meat_level_t *levels;
    int                 level_count;
} meat_type_t;

static const meat_level_t BEEF_LEVELS[] = {
    { "Rare", 52 },
    { "Medium Rare", 55 },
    { "Medium", 60 },
    { "Medium Well", 65 },
    { "Well Done", 70 },
};

static const meat_level_t LAMB_LEVELS[] = {
    { "Rare / UK Minimum", 52 },
    { "Medium Rare /US Minimum", 63 },
    { "Medium", 71 },
    { "Medium Well", 73 },
    { "Well Done", 77 },
};

static const meat_level_t PORK_LEVELS[] = {
    { "Medium / US Minimum", 63 },
    { "Medium Well", 70 },
    { "Well Done / UK Minimum", 75 },
};

static const meat_type_t MEAT_TYPES[] = {
    { "Beef", BEEF_LEVELS, 5 },
    { "Lamb", LAMB_LEVELS, 5 },
    { "Pork", PORK_LEVELS, 3 },
};
#define MEAT_TYPE_COUNT 3

// Poultry has a single food-safety target rather than a doneness
// preference, so it isn't part of the table above.
#define CHICKEN_SAFE_TARGET_C 74
