/*
 * Copyright (C) 2007-2024, Abel Cheung.
 * rifiuti2 is released under Revised BSD License.
 * Please see LICENSE file for more info.
 */

#include "config.h"

#include <locale.h>
#include <glib/gi18n.h>

#include "utils-conv.h"
#include "utils-error.h"
#include "utils-io.h"
#include "utils.h"
#include "utils-platform.h"

/* Our own error domain */

G_DEFINE_QUARK (rifiuti-fatal-error-quark, rifiuti_fatal_error)
G_DEFINE_QUARK (rifiuti-record-error-quark, rifiuti_record_error)

/* Common function signature for option callbacks */
#define DECL_OPT_CALLBACK(func)          \
static gboolean func (       \
    const gchar   *opt_name, \
    const gchar   *value,    \
    gpointer       data,     \
    GError       **error)

DECL_OPT_CALLBACK(_check_legacy_encoding);
DECL_OPT_CALLBACK(_set_output_path);
DECL_OPT_CALLBACK(_option_deprecated);
DECL_OPT_CALLBACK(_set_opt_delim);
DECL_OPT_CALLBACK(_set_opt_noheading);
DECL_OPT_CALLBACK(_set_opt_format);
DECL_OPT_CALLBACK(_show_ver_and_exit);

/* pre-declared out of laziness */

static int
_check_file_args (const char  *path,
                  GPtrArray   *list,
                  rbin_type    type,
                  bool        *isolated_index,
                  GError     **error);


/**
 * @brief More detailed OS version guess from artifacts
 * @note This is different from `detected_os_ver`, which only checks for
 * first few bytes. It is a more detailed breakdown, and for detection of
 * exact Windows version from various recycle bin artifacts.
 * @warning MUST match order of `os_strings` array except
 *          the `UNKNOWN` entry
 */
typedef enum
{
    OS_GUESS_UNKNOWN = -1,
    OS_GUESS_95,
    OS_GUESS_NT4,
    OS_GUESS_98,
    OS_GUESS_ME,
    OS_GUESS_2K,
    OS_GUESS_XP_03,
    OS_GUESS_2K_03,   /* Empty recycle bin, full detection impossible */
    OS_GUESS_VISTA,   /* includes everything up to 8.1 */
    OS_GUESS_10
} _os_guess;

/**
 * @brief Outputed string for OS detection from artifacts
 * @warning MUST match order of `_os_guess` enum
 */
static char *os_strings[] = {
    N_("Windows 95"),
    N_("Windows NT 4.0"),
    N_("Windows 98"),
    N_("Windows ME"),
    N_("Windows 2000"),
    N_("Windows XP or 2003"),
    N_("Windows 2000, XP or 2003"),
    N_("Windows Vista - 8.1"),
    N_("Windows 10 or above")
};


static out_fmt      output_format      = FORMAT_UNKNOWN;
static bool         no_heading         = false;
static gboolean     use_localtime      = FALSE;
static gboolean     live_mode          = FALSE;
static char        *delim              = NULL;
static char        *output_loc         = NULL;
static char       **fileargs           = NULL;
       GPtrArray   *allidxfiles        = NULL;
       bool         isolated_index     = false;
       char        *legacy_encoding    = NULL; /*!< INFO2 only, or upon request */
       metarecord  *meta               = NULL;


/* Options controlling output format */
static const GOptionEntry out_options[] = {
    {
        "delimiter", 't', 0,
        G_OPTION_ARG_CALLBACK, _set_opt_delim,
        N_("Field delimiter for TSV ['\\t' (TAB) if not given]"), N_("STRING")
    },
    {
        "no-heading", 'n', G_OPTION_FLAG_NO_ARG,
        G_OPTION_ARG_CALLBACK, _set_opt_noheading,
        N_("Don't show TSV column header and metadata"), NULL
    },
    {
        "xml", 'x', G_OPTION_FLAG_NO_ARG,
        G_OPTION_ARG_CALLBACK, _option_deprecated,
        N_("Deprecated, use '-f xml' in future"), NULL
    },
    {
        "format", 'f', 0,
        G_OPTION_ARG_CALLBACK, _set_opt_format,
        N_("'text' (default), 'xml' or 'json'"), N_("FORMAT")
    },
    { 0 }
};

/* Global options for program */
static const GOptionEntry main_options[] = {
    {
        "output", 'o', 0,
        G_OPTION_ARG_CALLBACK, _set_output_path,
        N_("Write output to FILE"), N_("FILE")
    },
    {
        "localtime", 'z', 0,
        G_OPTION_ARG_NONE, &use_localtime,
        N_("Present deletion time in time zone of local system (default is UTC)"),
        NULL
    },
    {
        "version", 'v', G_OPTION_FLAG_NO_ARG,
        G_OPTION_ARG_CALLBACK, _show_ver_and_exit,
        N_("Print version information and exit"), NULL
    },
    {
        G_OPTION_REMAINING, 0, 0,
        G_OPTION_ARG_FILENAME_ARRAY, &fileargs,
        N_("INFO2 file name"), NULL
    },
    { 0 }
};

/* Options only intended for INFO2 reader */
static const GOptionEntry rbinfile_options[] = {
    {
        "legacy-filename", 'l', 0,
        G_OPTION_ARG_CALLBACK, _check_legacy_encoding,
        N_("Show legacy (8.3) path if available and specify its CODEPAGE"),
        N_("CODEPAGE")
    },
    { 0 }
};

/* Options only intended for live system probation */
static const GOptionEntry live_options[] = {
    {
        "live", 0, 0,
        G_OPTION_ARG_NONE, &live_mode,
        N_("Inspect live system"), NULL
    },
    { 0 }
};

/* Following routines are command argument handling related */

static gboolean
_set_out_format    (out_fmt     desired_format,
                    GError    **error)
{
    extern struct _fmt_data fmt[];

    if (output_format == desired_format)
        return TRUE;

    if (output_format == FORMAT_UNKNOWN) {
        output_format = desired_format;
        return TRUE;
    }

    g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
        "Output was already set in %s, but later argument "
        "attempts to change to %s",
        fmt[output_format].friendly_name,
        fmt[desired_format].friendly_name);
    return FALSE;
}


static gboolean
_set_opt_format   (const gchar *opt_name,
                   const gchar *format,
                   gpointer     data,
                   GError     **error)
{
    UNUSED(opt_name);
    UNUSED(data);

    if (g_strcmp0 (format, "text") == 0)
        return _set_out_format (FORMAT_TEXT, error);
    else if (g_strcmp0 (format, "tsv") == 0)  // aliases
        return _set_out_format (FORMAT_TEXT, error);
    else if (g_strcmp0 (format, "csv") == 0)
        return _set_out_format (FORMAT_TEXT, error);
    else if (g_strcmp0 (format, "xml") == 0)
        return _set_out_format (FORMAT_XML, error);
    else if (g_strcmp0 (format, "json") == 0)
        return _set_out_format (FORMAT_JSON, error);
    else {
        g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
            "Illegal output format '%s'", format);
        return FALSE;
    }
}


/**
 * @brief Option callback for setting TSV header visibility
 * @return `FALSE` if option conflict exists, `TRUE` otherwise
 */
static gboolean
_set_opt_noheading (const gchar *opt_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
    UNUSED(opt_name);
    UNUSED(value);
    UNUSED(data);

    no_heading = true;

    return _set_out_format (FORMAT_TEXT, error);
}


/**
 * @brief Option callback for setting field delimiter in TSV output
 * @return `FALSE` if duplicate options are found, `TRUE` otherwise
 */
static gboolean
_set_opt_delim (const gchar *opt_name,
               const gchar *value,
               gpointer     data,
               GError     **error)
{
    UNUSED(opt_name);
    UNUSED(data);

    static bool seen = false;

    if (seen)
    {
        g_set_error_literal (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
            _("Multiple delimiter options disallowed."));
        return FALSE;
    }
    seen = true;

    delim = (*value) ? filter_escapes (value) : g_strdup ("");

    return _set_out_format (FORMAT_TEXT, error);
}


/**
 * @brief Option callback to set output file location
 * @return `FALSE` if duplicate options are found, or
 * output file location already exists. `TRUE` otherwise.
 */
static gboolean
_set_output_path (const gchar *opt_name,
                  const gchar *value,
                  gpointer     data,
                  GError     **error)
{
    UNUSED(opt_name);
    UNUSED(data);

    static bool seen = false;

    if (seen)
    {
        g_set_error_literal (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
            _("Multiple output destinations disallowed."));
        return FALSE;
    }
    seen = true;

    if ( *value == '\0' )
    {
        g_set_error_literal (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
            _("Empty output filename disallowed."));
        return FALSE;
    }

    if (g_file_test (value, G_FILE_TEST_EXISTS)) {
        g_set_error_literal (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
            _("Output destinations already exists."));
        return FALSE;
    }

    output_loc = g_strdup (value);
    return TRUE;
}


/**
 * @brief Emits warning when an argument is marked as deprecated
 * @return Always `TRUE`
 */
static gboolean
_option_deprecated (const gchar *opt_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
    UNUSED(value);
    UNUSED(data);
    if (strcmp (opt_name, "-x") == 0 || strcmp (opt_name, "--xml") == 0)
    {
        g_warning(_("Option '%s' is deprecated. Use '-f xml' in future."), opt_name);
        return _set_out_format (FORMAT_XML, error);
    }
    return TRUE;
}


/**
 * @brief Check if supplied legacy ANSI code page is valid
 * @return `TRUE` if supplied code page is usable, `FALSE` otherwise.
 * @note Code page is not validated against actual recycle bin record.
 */
static gboolean
_check_legacy_encoding (const gchar *opt_name,
                        const gchar *enc,
                        gpointer     data,
                        GError     **error)
{
    UNUSED(opt_name);
    UNUSED(data);

    static bool seen     = false;
    GError     *conv_err = NULL;

    if (seen)
    {
        g_set_error_literal (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
            _("Multiple encoding options disallowed."));
        return FALSE;
    }
    seen = true;

    if ( *enc == '\0' )
    {
        g_set_error_literal (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
            _("Empty encoding option disallowed."));
        return FALSE;
    }

    if (enc_is_ascii_compatible (enc, &conv_err))
    {
        legacy_encoding = g_strdup (enc);
        return TRUE;
    }

    /* everything below is error handling */

    if (g_error_matches (conv_err, G_CONVERT_ERROR, G_CONVERT_ERROR_NO_CONVERSION)) {
        g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
            _("'%s' encoding is not supported by glib library "
            "on this system.  If iconv program is present on "
            "system, use 'iconv -l' for a list of possible "
            "alternatives; otherwise check out following site for "
            "a list of probable encodings to use:\n\n\t%s"), enc,
#ifdef G_OS_WIN32
            "https://github.com/win-iconv/win-iconv/blob/master/win_iconv.c"
#else
            "https://www.gnu.org/software/libiconv/"
#endif
        );
    } else if (
        g_error_matches (conv_err, G_CONVERT_ERROR, G_CONVERT_ERROR_ILLEGAL_SEQUENCE) ||
        g_error_matches (conv_err, G_CONVERT_ERROR, G_CONVERT_ERROR_PARTIAL_INPUT)
    ) {
        g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
            _("'%s' is incompatible to any Windows code page."), enc);
    } else
        g_assert_not_reached ();

    g_clear_error (&conv_err);
    return FALSE;
}


/**
 * @brief Print program version with some text, then exit
 */
static gboolean
_show_ver_and_exit (const gchar *opt_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
    UNUSED (opt_name);
    UNUSED (value);
    UNUSED (data);
    UNUSED (error);

    g_print ("%s %s\n", PROJECT_NAME, PROJECT_VERSION);
    g_print ("%s\n\n", PROJECT_DESCRIPTION);
    /* TRANSLATOR COMMENT: %s is software name */
    g_print (_("%s is released under Revised BSD License.\n"), PROJECT_NAME);
    /* TRANSLATOR COMMENT: 1st argument is software name, 2nd is official URL */
    g_print (_("More information can be found on\n\n\t%s\n"),
        PROJECT_HOMEPAGE_URL);

    // OK I cheated, it is not returning at all.
    exit (EXIT_OK);
}


/**
 * @brief File argument check callback, after handling all arguments
 * @return `TRUE` if a unique file argument is used under common scenario,
 * or no file argument is provided in live mode. `FALSE` otherwise.
 */
static gboolean
_fileargs_handler (GOptionContext *context,
                   GOptionGroup   *group,
                   metarecord     *meta,
                   GError        **error)
{
    UNUSED (context);
    UNUSED (group);

    gsize fileargs_len = fileargs ? g_strv_length (fileargs) : 0;

    if (!live_mode)
    {
        if (fileargs_len != 1)
        {
            g_set_error_literal (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                _("Must specify exactly one file or folder argument."));
            return FALSE;
        }
        meta->filename = g_strdup (fileargs[0]);

        return _check_file_args (meta->filename, allidxfiles,
            meta->type, &isolated_index, error);
    }

    if (fileargs_len)
    {
        g_set_error_literal (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
            _("Live system probation must not be used together "
                "with file arguments."));
        return FALSE;
    }

#if (defined G_OS_WIN32 || defined __linux__)
    {
        meta->filename = g_strdup ("(current system)");

        GPtrArray *bindirs = enumerate_drive_bins (error);
        if (!bindirs)
        {
            char *reason = g_strdup ((*error)->message);
            g_clear_error (error);
            g_set_error (error, R2_FATAL_ERROR,
                R2_FATAL_ERROR_LIVE_UNSUPPORTED,
                _("Live probation unsupported under this system; "
                "requires running under Windows or WSL distribution.\n"
                "Failure reason: %s"), reason);
            g_free (reason);
            return FALSE;
        }

        for (gsize i = 0; i < bindirs->len; i++)
        {
            // Ignore errors, pretty common that some folders don't
            // exist or are empty.
            _check_file_args ((const char *)(bindirs->pdata[i]),
                allidxfiles, meta->type, NULL, NULL);
        }
        g_ptr_array_free (bindirs, TRUE);
    }
#endif

    return TRUE;
}


/**
 * @brief post-callback after handling all output related args
 * @return Always `TRUE`, denoting success. It never fails.
 */
static gboolean
_set_def_output_opts    (GOptionContext *context,
                         GOptionGroup   *group,
                         gpointer        data,
                         GError        **error)
{
    UNUSED (context);
    UNUSED (group);
    UNUSED (data);
    UNUSED (error);

    /* Fallback values after successful option parsing */
    if (delim == NULL)
        delim = g_strdup ("\t");

    if (output_format == FORMAT_UNKNOWN)
        output_format = FORMAT_TEXT;

    return TRUE;
}


/**
 * @brief Converts Windows FILETIME number to glib counterpart
 * @param win_filetime The FILETIME integer to be converted
 * @return `GDateTime` with UTC timezone
 */
GDateTime *
win_filetime_to_gdatetime (int64_t win_filetime)
{
    int64_t t;

    /* Let's assume we don't need subsecond time resolution */
    t = (win_filetime - 116444736000000000LL) / 10000000;

    g_debug ("FileTime -> Epoch: %" PRId64
        " -> %" PRId64, win_filetime, t);

    return g_date_time_new_from_unix_utc (t);
}


/**
 * @brief Prepare for glib option group setup
 * @param context Pointer to option context to be modified
 * @param type Recycle bin type; some options may or may not be available depending on type
 * @param meta Pointer to metadata structure
 */
static void
_opt_ctxt_setup (GOptionContext **context,
                 rbin_type        type)
{
    char         *desc_str;
    GOptionGroup *main_group, *output_group;

    /* FIXME Sneaky metadata modification! Think about cleaner way */
    meta->type = type;

    desc_str = g_strdup_printf (
        _("Usage help: %s\nBug report: %s\nMore info : %s"),
        PROJECT_TOOL_USAGE_URL,
        PROJECT_BUG_REPORT_URL,
        PROJECT_GH_PAGE);
    g_option_context_set_description (*context, desc_str);
    g_free (desc_str);

    /* main group */
    main_group = g_option_group_new (NULL, NULL, NULL, meta, NULL);

    g_option_group_add_entries (main_group, main_options);
    switch (type)
    {
        case RECYCLE_BIN_TYPE_FILE:
            g_option_group_add_entries (main_group, rbinfile_options);
            break;
        case RECYCLE_BIN_TYPE_DIR:
#if (defined G_OS_WIN32 || defined __linux__)
            g_option_group_add_entries (main_group, live_options);
#else
            UNUSED (live_options);
#endif
            break;
        default: break;
    }

    g_option_group_set_parse_hooks (main_group, NULL,
        (GOptionParseFunc) _fileargs_handler);
    g_option_context_set_main_group (*context, main_group);

    /* output format arg group */
    output_group = g_option_group_new ("format",
        _("Output format options:"),
        N_("Show output formatting options"), NULL, NULL);

    g_option_group_add_entries (output_group, out_options);
    g_option_group_set_parse_hooks (
        output_group, NULL, _set_def_output_opts);
    g_option_context_add_group (*context, output_group);

    g_option_context_set_help_enabled (*context, TRUE);
}

/**
 * @brief Process command line arguments
 * @param context Reference of option context pointer
 * @param argv Reference of command line `argv`
 * @param error Reference of `GError` pointer to store errors
 * @return `TRUE` if argument parsing succeeds.
 * `FALSE` on failure, and sets `error` as well.
 */
static bool
_opt_ctxt_parse (GOptionContext **context,
                 char          ***argv,
                 GError         **error)
{
    gsize     argc;
    char    **argv_u8;

#ifdef G_OS_WIN32
    argv_u8 = g_win32_get_command_line ();
    UNUSED (argv);
#else
    argv_u8 = g_strdupv (*argv);
#endif

    argc = g_strv_length (argv_u8);
    if (argc == 1) {
        argv_u8 = g_realloc_n (argv_u8, argc + 2, sizeof(gpointer));
        argv_u8[argc++] = "--help-all";
        argv_u8[argc] = (void *) NULL;
#ifdef G_OS_WIN32
        g_set_print_handler (gui_message);
#endif
    }

    {
        char *args_str = g_strjoinv("|", argv_u8);
        g_debug("Calling argv_u8 (%zu): %s", argc, args_str);
        g_free(args_str);
    }

    g_option_context_parse_strv (*context, &argv_u8, error);
    g_option_context_free (*context);
    g_strfreev (argv_u8);

    return (*error == NULL);
}


/**
 * @brief Free all fields used in a single recycle bin record
 * @param record Pointer to the record structure
 */
static void
_free_record_cb (rbin_struct *record)
{
    g_free (record->index_s);
    g_date_time_unref (record->deltime);
    if (record->raw_uni_path)
        g_string_free (record->raw_uni_path, TRUE);
    if (record->raw_legacy_path)
        g_string_free (record->raw_legacy_path, TRUE);
    g_clear_error (&record->error);
    g_free (record);
}


/**
 * @brief Initialize program setup
 */
bool
rifiuti_init (rbin_type  type,
              char      *usage_param,
              char      *usage_summary,
              char    ***argv,
              GError   **error)
{
    GOptionContext *context;

    setlocale (LC_ALL, "");
    init_handles ();

    /* Initialize metadata struct */
    meta = g_malloc0 (sizeof (metarecord));
    meta->records = g_ptr_array_new ();
    g_ptr_array_set_free_func (meta->records, (GDestroyNotify) _free_record_cb);
    meta->invalid_records = g_hash_table_new_full (
        g_str_hash,
        g_str_equal,
        (GDestroyNotify) g_free,
        (GDestroyNotify) g_error_free
    );

    // Other global structures
    allidxfiles = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);

    /* Parse command line arguments and generate help */
    context = g_option_context_new (usage_param);
    g_option_context_set_summary (context, usage_summary);
    _opt_ctxt_setup (&context, type);

    return _opt_ctxt_parse (&context, argv, error);
}


/**
 * @brief Scan folder and add all index files for parsing
 * @param list Pointer to file list to be modified
 * @param path The folder to scan
 * @param error Pointer to `GError` for error reporting
 * @return `TRUE` on success, `FALSE` if folder can't be opened
 */
static bool
_populate_index_file_list (GPtrArray   *list,
                           const char  *path,
                           GError     **error)
{
    GDir           *dir;
    const char     *direntry;
    GPatternSpec   *pattern1, *pattern2;

    // g_dir_open() returns cryptic error message or even succeeds on Windows,
    // when in fact the directory content is inaccessible.
#ifdef G_OS_WIN32
    if ( !can_list_win32_folder (path, error) ) {
        return false;
    }
#endif

    if (NULL == (dir = g_dir_open (path, 0, error)))
        return false;

    pattern1 = g_pattern_spec_new ("$I??????.*");
    pattern2 = g_pattern_spec_new ("$I??????");

    while ((direntry = g_dir_read_name (dir)) != NULL)
    {
#if GLIB_CHECK_VERSION (2, 70, 0)
        if (!g_pattern_spec_match_string (pattern1, direntry) &&
            !g_pattern_spec_match_string (pattern2, direntry))
            continue;
#else /* glib < 2.70 */
        if (!g_pattern_match_string (pattern1, direntry) &&
            !g_pattern_match_string (pattern2, direntry))
            continue;
#endif
        g_ptr_array_add (list,
            g_build_filename (path, direntry, NULL));
    }

    g_dir_close (dir);

    g_pattern_spec_free (pattern1);
    g_pattern_spec_free (pattern2);

    return true;
}


/**
 * @brief Search for desktop.ini in folder for hint of recycle bin
 * @param path The searched path
 * @return `TRUE` if `desktop.ini` found to contain recycle bin
 *         identifier, `FALSE` otherwise
 */
static bool
_found_desktop_ini (const char *path)
{
    char *filename = NULL, *content = NULL, *found = NULL;

    filename = g_build_filename (path, "desktop.ini", NULL);
    if (!g_file_test (filename, G_FILE_TEST_IS_REGULAR))
    {
        g_free (filename);
        return false;
    }

    if (g_file_get_contents (filename, &content, NULL, NULL))
        /* Don't bother parsing, we don't use the content at all */
        found = strstr (content, RECYCLE_BIN_CLSID);

    g_free (content);
    g_free (filename);
    return (found != NULL);
}


/**
 * @brief Guess Windows version which generated recycle bin index file
 * @param meta Pointer to metadata structure
 * @return Enum constant representing approximate Windows version range
 */
static _os_guess
_guess_windows_ver (const metarecord *meta)
{
    if (meta->type == RECYCLE_BIN_TYPE_DIR) {
        /*
        * No attempt is made to distinguish difference for Vista - 8.1.
        * The corrupt filesize artifact on Vista can't be reproduced,
        * therefore must be very rare.
        */
        switch (meta->version)
        {
            case VERSION_VISTA: return OS_GUESS_VISTA;
            case VERSION_WIN10: return OS_GUESS_10;
            default:            return OS_GUESS_UNKNOWN;
        }
    }

    /* INFO2 only below */

    switch (meta->version)
    {
        case VERSION_WIN95: return OS_GUESS_95;
        case VERSION_WIN98: return OS_GUESS_98;
        case VERSION_NT4  : return OS_GUESS_NT4;
        case VERSION_ME_03:
            /* TODO use symbolic name when 2 versions are merged */
            if (meta->recordsize == 280)
                return OS_GUESS_ME;

            if (meta->records->len == 0)
                return OS_GUESS_2K_03;

            return meta->fill_junk ? OS_GUESS_2K : OS_GUESS_XP_03;

        /* Not using OS_GUESS_UNKNOWN, INFO2 ceased to be used so
           detection logic won't change in future */
        default: g_assert_not_reached();
    }

    // Should be impossible to reach here, but for shutting up
    // diagnostics warning...
    return OS_GUESS_UNKNOWN;
}

/**
 * @brief Add potentially valid file(s) to list
 * @param path The file or folder to be checked
 * @param list A `GPtrArray` to store potential index files
 * @param type Recycle bin type
 * @param isolated_index Pointer to `gboolean`, indicating whether
 * the concerned `path` is a single `$Recycle.bin` type index
 * taken out of its original folder. Can be `NULL`, which means
 * this check is not performed.
 * @param error A `GError` pointer to store potential problems
 * @return `TRUE` if input file/dir is valid, `FALSE` otherwise
 * @attention Successful result does not imply files are appended
 * to list, which is the case for empty recycle bin
 */
static gboolean
_check_file_args (const char  *path,
                  GPtrArray   *list,
                  rbin_type    type,
                  bool        *isolated_index,
                  GError     **error)
{
    g_debug ("Start checking path '%s'...", path);

    g_return_val_if_fail (path != NULL, FALSE);
    g_return_val_if_fail (list != NULL, FALSE);

    if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
            _("'%s' does not exist."), path);
        return FALSE;
    }

    if ((type == RECYCLE_BIN_TYPE_DIR) &&
        g_file_test (path, G_FILE_TEST_IS_DIR))
    {
        if ( ! _populate_index_file_list (list, path, error) )
            return FALSE;
        /*
         * last ditch effort: search for desktop.ini. Just print empty content
         * representing empty recycle bin if found.
         */
        if (list->len == 0 && ! _found_desktop_ini (path))
        {
            g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                _("No files with name pattern '%s' "
                "are found in directory."), "$Ixxxxxx.*");
            return FALSE;
        }
    }
    else if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
        if (isolated_index && (type == RECYCLE_BIN_TYPE_DIR)) {
            char *parent_dir = g_path_get_dirname (path);
            *isolated_index = ! _found_desktop_ini (parent_dir);
            g_free (parent_dir);
        }
        g_ptr_array_add (list, g_strdup (path));
    }
    else
    {
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
            (type == RECYCLE_BIN_TYPE_DIR) ?
            _("'%s' is not a normal file or directory.") :
            _("'%s' is not a normal file."), path);
        return FALSE;
    }
    return TRUE;
}

void
do_parse_records (ParseIdxFunc func)
{
    g_ptr_array_foreach (allidxfiles, (GFunc) func, meta);
}


/**
 * @brief Print preamble and column header for TSV output
 * @param meta Pointer to metadata structure
 */
static void
_print_text_header (const metarecord *meta)
{
    {
        char *rbin_path = g_filename_display_name (meta->filename);
        g_print (_("Recycle bin path: '%s'\n"), rbin_path);
        g_free (rbin_path);
    }

    if (meta->version == VERSION_NOT_FOUND) {
        g_print ("%s\n", _("Version: ??? (empty folder)"));
    } else {
        g_print (_("Version: %" PRIu64 "\n"), meta->version);
    }

    if (( meta->type == RECYCLE_BIN_TYPE_FILE ) && meta->total_entry)
    {
        g_print (_("Total entries ever existed: %d"), meta->total_entry);
        g_print ("\n");
    }

#if (defined G_OS_WIN32 || defined __linux__)
    if (live_mode)
    {
        char *product_name = windows_product_name();

        if (product_name) {
            g_print (_("OS: %s"), product_name);
            g_free (product_name);
        } else {
            g_print ("%s", _("OS detection failed"));
        }
    }
    else
#endif
    {
        _os_guess g = _guess_windows_ver (meta);

        if (g == OS_GUESS_UNKNOWN)
            g_print ("%s", _("OS detection failed"));
        else
            g_print (_("OS Guess: %s"), gettext (os_strings[g]) );
    }

    g_print ("\n");

    // Deletion time for each entry may or may not be under DST.
    // Results have not been verified.
    {
        GDateTime *now;
        char      *tzname = NULL, *tznumeric = NULL;

        now = use_localtime ? g_date_time_new_now_local ():
                              g_date_time_new_now_utc   ();

#ifdef G_OS_WIN32
        if (use_localtime)
            tzname = get_win_timezone_name ();
#endif
        if (tzname == NULL)
            tzname = g_date_time_format (now, "%Z");

        tznumeric = g_date_time_format (now, "%z");

        g_print (_("Time zone: %s [%s]"), tzname, tznumeric);
        g_print ("\n");

        g_date_time_unref (now);
        g_free (tzname);
        g_free (tznumeric);
    }

    g_print ("\n");

    {
        char *fields[] = {
            /* TRANSLATOR COMMENT: appears in column header */
            N_("Index"), N_("Deleted Time"), N_("Gone?"), N_("Size"), N_("Path"), NULL
        };
        char *headerline = g_strjoinv (delim, fields);
        g_print ("%s\n", headerline);
        g_free (headerline);
    }
}


/**
 * @brief Print preamble for XML output
 * @param meta Pointer to metadata structure
 */
static void
_print_xml_header (const metarecord *meta)
{
    GString *result;

    result = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");

    g_string_append_printf (result,
        "<recyclebin format=\"%s\"",
        ( meta->type == RECYCLE_BIN_TYPE_FILE ) ? "file" : "dir");

    if (meta->version >= 0)  /* can be found and not error */
        g_string_append_printf (result,
            " version=\"%" PRId64 "\"",
            meta->version);

    if (meta->type == RECYCLE_BIN_TYPE_FILE && meta->total_entry > 0)
        g_string_append_printf (result,
            " ever_existed=\"%" PRIu32 "\"",
            meta->total_entry);

    result = g_string_append (result, ">\n");

    {
        char *rbin_path = g_filename_display_name (meta->filename);
        g_string_append_printf (result,
            "  <filename><![CDATA[%s]]></filename>\n",
            rbin_path);
        g_free (rbin_path);
    }

    g_print ("%s", result->str);
    g_string_free (result, TRUE);
}


/**
 * @brief Print preamble for JSON output
 * @param meta Pointer to metadata structure
 */
static void
_print_json_header (const metarecord *meta)
{
    g_print ("{\n  \"format\": \"%s\",\n",
        (meta->type == RECYCLE_BIN_TYPE_FILE) ? "file" : "dir");

    if (meta->version >= 0)  /* can be found and not error */
        g_print ("  \"version\": %" PRId64 ",\n", meta->version);
    else
        g_print ("  \"version\": null,\n");

    if (meta->type == RECYCLE_BIN_TYPE_FILE && meta->total_entry > 0)
        g_print ("  \"ever_existed\": %" PRIu32 ",\n", meta->total_entry);

    {
        char *s = g_filename_display_name (meta->filename);
        char *rbin_path = json_escape (s);
        g_print ("  \"path\": \"%s\",\n", rbin_path);
        g_free (s);
        g_free (rbin_path);
    }

    g_print ("  \"records\": [\n");
}


static void
_print_text_record   (rbin_struct        *record,
                      const metarecord   *meta)
{
    char         *output, **header;
    GString      *src;
    GDateTime    *dt;
    extern struct _fmt_data fmt[];

    g_return_if_fail (record != NULL);

    header = (char **) g_malloc0_n (6, sizeof(gpointer));

    header[0] = (meta->type == RECYCLE_BIN_TYPE_FILE) ?
        g_strdup_printf ("%" PRIu32, record->index_n) :
        g_strdup (record->index_s);

    dt = use_localtime ? g_date_time_to_local (record->deltime):
                         g_date_time_ref      (record->deltime);
    header[1] = g_date_time_format (dt, "%F %T");

    header[2] = g_strdup(fmt[FORMAT_TEXT].gone_outtext[record->gone]);

    header[3] = (record->filesize == G_MAXUINT64) ?  // faulty
        g_strdup ("???") :
        g_strdup_printf ("%" PRIu64, record->filesize);

    src = legacy_encoding ? record->raw_legacy_path :
                            record->raw_uni_path    ;
    header[4] = conv_path_to_utf8_with_tmpl (src,
        legacy_encoding, FORMAT_TEXT, NULL, &record->error);
    if (! header[4])
        header[4] = g_strdup ("???");

    output = g_strjoinv (delim, header);
    g_print ("%s\n", output);

    g_free (output);
    g_date_time_unref (dt);
    g_strfreev (header);
}


static void
_print_xml_record   (rbin_struct        *record,
                     const metarecord   *meta)
{
    extern struct _fmt_data fmt[];
    char         *path, *dt_str;
    GDateTime    *dt;
    GString      *s, *src;

    g_return_if_fail (record != NULL);

    s = g_string_new ("  <record");

    if (meta->type == RECYCLE_BIN_TYPE_FILE)
        g_string_append_printf (s, " index=\"%" PRIu32 "\"", record->index_n);
    else
        g_string_append_printf (s, " index=\"%s\"", record->index_s);

    if (use_localtime)
    {
        dt = g_date_time_to_local (record->deltime);
        dt_str = g_date_time_format (dt, "%FT%T%z");
    }
    else
    {
        dt = g_date_time_ref (record->deltime);
        dt_str = g_date_time_format (dt, "%FT%TZ");
    }
    g_string_append_printf (s, " time=\"%s\"", dt_str);

    g_string_append_printf (s, " gone=\"%s\"",
        fmt[FORMAT_XML].gone_outtext[record->gone]);

    if (record->filesize == G_MAXUINT64)  // faulty
        g_string_append_printf (s, " size=\"-1\"");
    else
        g_string_append_printf (s,
            " size=\"%" PRIu64 "\"", record->filesize);

    // Still need to be converted despite using CDATA,
    // otherwise could be writing garbage output
    src = legacy_encoding ? record->raw_legacy_path :
                            record->raw_uni_path    ;
    path = conv_path_to_utf8_with_tmpl (src,
        legacy_encoding, FORMAT_XML, NULL, &record->error);

    if (path)
        g_string_append_printf (s, ">\n"
            "    <path><![CDATA[%s]]></path>\n"
            "  </record>\n", path);
    else
        s = g_string_append (s, ">\n    <path/>\n  </record>\n");

    g_print ("%s", s->str);
    g_string_free (s, TRUE);

    g_date_time_unref (dt);
    g_free (path);
    g_free (dt_str);
}


static void
_print_json_record   (rbin_struct        *record,
                      const metarecord   *meta)
{
    extern struct _fmt_data fmt[];
    char         *path, *dt_str;
    GDateTime    *dt;
    GString      *src, *s;

    g_return_if_fail (record != NULL);

    s = g_string_new ("    {");

    if (meta->type == RECYCLE_BIN_TYPE_FILE)
        g_string_append_printf (s, "\"index\": %" PRIu32, record->index_n);
    else
        g_string_append_printf (s, "\"index\": \"%s\"", record->index_s);

    if (use_localtime)
    {
        dt = g_date_time_to_local (record->deltime);
        dt_str = g_date_time_format (dt, "%FT%T%z");
    }
    else
    {
        dt = g_date_time_ref (record->deltime);
        dt_str = g_date_time_format (dt, "%FT%TZ");
    }
    g_string_append_printf (s, ", \"time\": \"%s\"", dt_str);

    g_string_append_printf (s, ", \"gone\": %s",
        fmt[FORMAT_JSON].gone_outtext[record->gone]);

    if (record->filesize == G_MAXUINT64)  // faulty
        g_string_append_printf (s, ", \"size\": null");
    else
        g_string_append_printf (s,
            ", \"size\": %" PRIu64, record->filesize);

    src = legacy_encoding ? record->raw_legacy_path :
                            record->raw_uni_path    ;
    path = conv_path_to_utf8_with_tmpl (src, legacy_encoding,
        FORMAT_JSON, &json_escape, &record->error);

    if (path)
        g_string_append_printf (s, ", \"path\": \"%s\"},\n", path);
    else
        s = g_string_append (s, ", \"path\": null},\n");

    g_print ("%s", s->str);

    g_date_time_unref (dt);
    g_free (path);
    g_free (dt_str);
    g_string_free (s, TRUE);
}


static void
_print_xml_footer (void)
{
    g_print ("%s", "</recyclebin>\n");
}


static void
_print_json_footer (void)
{
    g_print ("  ]\n}\n");
}


/**
 * @brief Dump all results to screen or designated output file
 * @param error Reference of `GError` pointer to store potential problem
 * @return `TRUE` if output writing is successful, `FALSE` otherwise
 */
bool
dump_content (GError **error)
{
    void (*print_header_func)(const metarecord *);
    void (*print_record_func)(rbin_struct *, const metarecord *);
    void (*print_footer_func)();

    // TODO use g_file_set_contents_full in glib 2.66
    if (output_loc && ! get_tempfile (error))
            return false;

    switch (output_format)
    {
        case FORMAT_TEXT:
            print_header_func = no_heading ?
                NULL : &_print_text_header;
            print_record_func = &_print_text_record;
            print_footer_func = NULL;
            break;
        case FORMAT_XML:
            print_header_func = &_print_xml_header;
            print_record_func = &_print_xml_record;
            print_footer_func = &_print_xml_footer;
            break;
        case FORMAT_JSON:
            print_header_func = &_print_json_header;
            print_record_func = &_print_json_record;
            print_footer_func = &_print_json_footer;
            break;

        default: g_assert_not_reached();
    }

    if (print_header_func != NULL)
        (*print_header_func) (meta);
    g_ptr_array_foreach (meta->records, (GFunc) print_record_func, meta);
    if (print_footer_func != NULL)
        (*print_footer_func) ();

    if (output_loc)
        return clean_tempfile (output_loc, error);
    else
        return true;
}


static void
_dump_rec_error   (rbin_struct  *record,
                   bool         *flag)
{
    g_return_if_fail (record);

    if (! record->error)
        return;

    if (! *flag)
    {
        *flag = true;
        g_printerr ("\n%s\n", _("Error occurred in following record:"));
    }

    if (record->index_n)
        g_printerr ("%2u: %s\n", record->index_n,
                record->error->message);
    else
        g_printerr ("%s: %s\n", record->index_s,
                record->error->message);

    return;
}


/**
 * @brief Handle global and record errors before quitting
 * @param error The global `GError` to process
 * @return program exit code
 */
exitcode
_get_exit_code   (const GError   *error)
{
    exitcode code = EXIT_OK;

    if (error == NULL)
        return code;

    g_printerr ("Fatal error: %s\n", error->message);

    if (error->domain == G_OPTION_ERROR)
        code = EXIT_ERR_ARG;
    else if (error->domain == G_FILE_ERROR)
        code = EXIT_ERR_OPEN_FILE;
    else if (g_error_matches (error,
        R2_FATAL_ERROR, R2_FATAL_ERROR_ILLEGAL_DATA))
        code = EXIT_ERR_ILLEGAL_DATA;
    else if (g_error_matches (error,
        R2_FATAL_ERROR, R2_FATAL_ERROR_TEMPFILE))
        code = EXIT_ERR_WRITE_FILE;
    else if (g_error_matches (error,
        R2_FATAL_ERROR, R2_FATAL_ERROR_LIVE_UNSUPPORTED))
        code = EXIT_ERR_NO_LIVE;
    else {
        g_critical ("Error not handled: quark = %s, code = %d",
            g_quark_to_string (error->domain), error->code);
        code = EXIT_ERR_UNHANDLED;
    }

    return code;
}


bool
_has_record_error   (void)
{
    bool flag = false;  // Determine occasion to print headline
    GHashTableIter iter;
    gpointer key, val;

    if (g_hash_table_size (meta->invalid_records))
    {
        flag = true;
        g_hash_table_iter_init (&iter, meta->invalid_records);
        g_printerr ("%s\n", _("Error occurred in following record:"));

        while (g_hash_table_iter_next (&iter, &key, &val))
        {
            char *record_id = (char *) key;

            if (*record_id == '|') {
                char **frags = g_strsplit (record_id, "|", 0);
                record_id = g_strdup_printf ("byte range %s - %s",
                        frags[1], frags[2]);
                g_strfreev (frags);
            } else
                record_id = g_strdup (record_id);
            g_printerr ("%s: %s\n", record_id,
                    ((GError *)val)->message);
            g_free (record_id);
        }
    }

    g_ptr_array_foreach (meta->records,
            (GFunc) _dump_rec_error, &flag);

    return flag;
}


/**
 * @brief Dump error and perform final cleanup
 * @param error The global `GError` to process
 * @return program exit code
 */
exitcode
rifiuti_cleanup   (GError   **error)
{
    exitcode code = EXIT_OK;

    g_return_val_if_fail (error != NULL, EXIT_ERR_UNHANDLED);

    code = _get_exit_code ((const GError *) (*error));
    g_clear_error (error);

    if (_has_record_error () && code == EXIT_OK)
        code = EXIT_ERR_DUBIOUS_DATA;

    g_debug ("Final cleanup...");

    g_ptr_array_unref (meta->records);
    g_hash_table_destroy (meta->invalid_records);
    g_free (meta->filename);
    g_free (meta);

    g_ptr_array_free (allidxfiles, TRUE);
    g_strfreev (fileargs);
    g_free (output_loc);
    g_free (legacy_encoding);
    g_free (delim);

    close_handles ();

#ifdef G_OS_WIN32
    cleanup_windows_res ();
#endif

    return code;
}


void
hexdump    (void     *start,
            size_t    size)
{
    GString *s = g_string_new ("");
    size_t i = 0;
    while (true)
    {
        if (i % 16 == 0)
        {
            if (s->len > 0)
            {
                g_debug ("%s", s->str);
                s = g_string_assign (s, "");
            }
            g_string_append_printf (s, "%04zX    ", i);
        }
        if (i >= size)
            break;
        g_string_append_printf (s, "%02" PRIX8 " ", *(uint8_t *) (start+i));
        i++;
    }

    g_string_free (s, TRUE);
}
