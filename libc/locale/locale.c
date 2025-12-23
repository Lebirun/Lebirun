#include <locale.h>
#include <langinfo.h>
#include <stddef.h>
#include <string.h>

static struct lconv default_lconv = {
    .decimal_point = ".",
    .thousands_sep = "",
    .grouping = "",
    .int_curr_symbol = "",
    .currency_symbol = "",
    .mon_decimal_point = "",
    .mon_thousands_sep = "",
    .mon_grouping = "",
    .positive_sign = "",
    .negative_sign = "",
    .int_frac_digits = 127,
    .frac_digits = 127,
    .p_cs_precedes = 127,
    .p_sep_by_space = 127,
    .n_cs_precedes = 127,
    .n_sep_by_space = 127,
    .p_sign_posn = 127,
    .n_sign_posn = 127,
    .int_p_cs_precedes = 127,
    .int_p_sep_by_space = 127,
    .int_n_cs_precedes = 127,
    .int_n_sep_by_space = 127,
    .int_p_sign_posn = 127,
    .int_n_sign_posn = 127
};

static char current_locale[] = "C";

char *setlocale(int category, const char *locale) {
    (void)category;
    if (locale == NULL) {
        return current_locale;
    }
    return current_locale;
}

struct lconv *localeconv(void) {
    return &default_lconv;
}

locale_t newlocale(int category_mask, const char *locale, locale_t base) {
    (void)category_mask;
    (void)locale;
    (void)base;
    return (locale_t)1;
}

locale_t duplocale(locale_t locobj) {
    (void)locobj;
    return (locale_t)1;
}

void freelocale(locale_t locobj) {
    (void)locobj;
}

locale_t uselocale(locale_t newloc) {
    (void)newloc;
    return (locale_t)1;
}

static const char *langinfo_strings[] = {
    "ANSI_X3.4-1968",
    "%a %b %e %H:%M:%S %Y",
    "%m/%d/%y",
    "%H:%M:%S",
    "%I:%M:%S %p",
    "AM",
    "PM",
    "Sunday",
    "Monday",
    "Tuesday",
    "Wednesday",
    "Thursday",
    "Friday",
    "Saturday",
    "Sun",
    "Mon",
    "Tue",
    "Wed",
    "Thu",
    "Fri",
    "Sat",
    "January",
    "February",
    "March",
    "April",
    "May",
    "June",
    "July",
    "August",
    "September",
    "October",
    "November",
    "December",
    "Jan",
    "Feb",
    "Mar",
    "Apr",
    "May",
    "Jun",
    "Jul",
    "Aug",
    "Sep",
    "Oct",
    "Nov",
    "Dec",
    "",
    "",
    "",
    "",
    "",
    ".",
    "",
    "^[yY]",
    "^[nN]",
    ""
};

char *nl_langinfo(nl_item item) {
    if (item < 0 || item > CRNCYSTR) {
        return "";
    }
    return (char *)langinfo_strings[item];
}

char *nl_langinfo_l(nl_item item, locale_t locale) {
    (void)locale;
    return nl_langinfo(item);
}
