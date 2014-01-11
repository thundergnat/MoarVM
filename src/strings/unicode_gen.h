
#define MVMNUMUNICODEEXTENTS 85


struct MVMUnicodeNamedValue {
    const char *name;
    MVMint32 value;
};
#define num_unicode_property_keypairs 4426

MVMint32 MVM_unicode_is_in_block(MVMThreadContext *tc, MVMString *str, MVMint64 pos, MVMString *block);

#define MVMCODEPOINTNAMESCOUNT 43833
#define MVMNUMPROPERTYCODES 83


#define num_unicode_property_value_keypairs 2580

typedef enum {
    MVM_UNICODE_PROPERTY_DECOMP_SPEC = 1,
    MVM_UNICODE_PROPERTY_CASE_CHANGE_INDEX = 2,
    MVM_UNICODE_PROPERTY_NUMERIC_VALUE = 3,
    MVM_UNICODE_PROPERTY_CASE_FOLDING = 4,
    MVM_UNICODE_PROPERTY_BIDI_MIRRORING_GLYPH = 5,
    MVM_UNICODE_PROPERTY_BLOCK = 6,
    MVM_UNICODE_PROPERTY_AGE = 7,
    MVM_UNICODE_PROPERTY_SCRIPT = 8,
    MVM_UNICODE_PROPERTY_CANONICAL_COMBINING_CLASS = 9,
    MVM_UNICODE_PROPERTY_JOINING_GROUP = 10,
    MVM_UNICODE_PROPERTY_BIDI_CLASS = 11,
    MVM_UNICODE_PROPERTY_WORD_BREAK = 12,
    MVM_UNICODE_PROPERTY_JOINING_TYPE = 13,
    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY = 14,
    MVM_UNICODE_PROPERTY_DECOMPOSITION_TYPE = 15,
    MVM_UNICODE_PROPERTY_LINE_BREAK = 16,
    MVM_UNICODE_PROPERTY_SENTENCE_BREAK = 17,
    MVM_UNICODE_PROPERTY_GRAPHEME_CLUSTER_BREAK = 18,
    MVM_UNICODE_PROPERTY_HANGUL_SYLLABLE_TYPE = 19,
    MVM_UNICODE_PROPERTY_NUMERIC_TYPE = 20,
    MVM_UNICODE_PROPERTY_NFKC_QC = 21,
    MVM_UNICODE_PROPERTY_DIGIT = 22,
    MVM_UNICODE_PROPERTY_NFC_QC = 23,
    MVM_UNICODE_PROPERTY_NUMERIC_VALUE_DENOMINATOR = 24,
    MVM_UNICODE_PROPERTY_NUMERIC_VALUE_NUMERATOR = 25,
    MVM_UNICODE_PROPERTY_ASCII_HEX_DIGIT = 26,
    MVM_UNICODE_PROPERTY_ALPHABETIC = 27,
    MVM_UNICODE_PROPERTY_BIDI_CONTROL = 28,
    MVM_UNICODE_PROPERTY_BIDI_MIRRORED = 29,
    MVM_UNICODE_PROPERTY_CASE_FOLDING_SIMPLE = 30,
    MVM_UNICODE_PROPERTY_CASE_IGNORABLE = 31,
    MVM_UNICODE_PROPERTY_CASED = 32,
    MVM_UNICODE_PROPERTY_CHANGES_WHEN_CASEFOLDED = 33,
    MVM_UNICODE_PROPERTY_CHANGES_WHEN_CASEMAPPED = 34,
    MVM_UNICODE_PROPERTY_CHANGES_WHEN_LOWERCASED = 35,
    MVM_UNICODE_PROPERTY_CHANGES_WHEN_NFKC_CASEFOLDED = 36,
    MVM_UNICODE_PROPERTY_CHANGES_WHEN_TITLECASED = 37,
    MVM_UNICODE_PROPERTY_CHANGES_WHEN_UPPERCASED = 38,
    MVM_UNICODE_PROPERTY_DASH = 39,
    MVM_UNICODE_PROPERTY_DEFAULT_IGNORABLE_CODE_POINT = 40,
    MVM_UNICODE_PROPERTY_DEPRECATED = 41,
    MVM_UNICODE_PROPERTY_DIACRITIC = 42,
    MVM_UNICODE_PROPERTY_EXTENDER = 43,
    MVM_UNICODE_PROPERTY_FULL_COMPOSITION_EXCLUSION = 44,
    MVM_UNICODE_PROPERTY_GRAPHEME_BASE = 45,
    MVM_UNICODE_PROPERTY_GRAPHEME_EXTEND = 46,
    MVM_UNICODE_PROPERTY_GRAPHEME_LINK = 47,
    MVM_UNICODE_PROPERTY_HEX_DIGIT = 48,
    MVM_UNICODE_PROPERTY_HYPHEN = 49,
    MVM_UNICODE_PROPERTY_IDS_BINARY_OPERATOR = 50,
    MVM_UNICODE_PROPERTY_IDS_TRINARY_OPERATOR = 51,
    MVM_UNICODE_PROPERTY_ID_CONTINUE = 52,
    MVM_UNICODE_PROPERTY_ID_START = 53,
    MVM_UNICODE_PROPERTY_IDEOGRAPHIC = 54,
    MVM_UNICODE_PROPERTY_JOIN_CONTROL = 55,
    MVM_UNICODE_PROPERTY_LOGICAL_ORDER_EXCEPTION = 56,
    MVM_UNICODE_PROPERTY_LOWERCASE = 57,
    MVM_UNICODE_PROPERTY_MATH = 58,
    MVM_UNICODE_PROPERTY_NFD_QC = 59,
    MVM_UNICODE_PROPERTY_NFKD_QC = 60,
    MVM_UNICODE_PROPERTY_NONCHARACTER_CODE_POINT = 61,
    MVM_UNICODE_PROPERTY_OTHER_ALPHABETIC = 62,
    MVM_UNICODE_PROPERTY_OTHER_DEFAULT_IGNORABLE_CODE_POINT = 63,
    MVM_UNICODE_PROPERTY_OTHER_GRAPHEME_EXTEND = 64,
    MVM_UNICODE_PROPERTY_OTHER_ID_CONTINUE = 65,
    MVM_UNICODE_PROPERTY_OTHER_ID_START = 66,
    MVM_UNICODE_PROPERTY_OTHER_LOWERCASE = 67,
    MVM_UNICODE_PROPERTY_OTHER_MATH = 68,
    MVM_UNICODE_PROPERTY_OTHER_UPPERCASE = 69,
    MVM_UNICODE_PROPERTY_PATTERN_SYNTAX = 70,
    MVM_UNICODE_PROPERTY_PATTERN_WHITE_SPACE = 71,
    MVM_UNICODE_PROPERTY_QUOTATION_MARK = 72,
    MVM_UNICODE_PROPERTY_RADICAL = 73,
    MVM_UNICODE_PROPERTY_STERM = 74,
    MVM_UNICODE_PROPERTY_SOFT_DOTTED = 75,
    MVM_UNICODE_PROPERTY_TERMINAL_PUNCTUATION = 76,
    MVM_UNICODE_PROPERTY_UNIFIED_IDEOGRAPH = 77,
    MVM_UNICODE_PROPERTY_UPPERCASE = 78,
    MVM_UNICODE_PROPERTY_VARIATION_SELECTOR = 79,
    MVM_UNICODE_PROPERTY_WHITE_SPACE = 80,
    MVM_UNICODE_PROPERTY_XID_CONTINUE = 81,
    MVM_UNICODE_PROPERTY_XID_START = 82,
} MVM_unicode_property_codes;