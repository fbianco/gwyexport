/*
 *  gwyexport.c
 *  Copyright © 2012 François Bianco
 *  Email: francois.bianco@unige.ch
 *
 *  This code is available under the GPL v3 or any later version
 *
 *  Based on gwyexport.c from P. Rahe, 30.05.2012, v0.1
 *  Copyright (C) 2010 Philipp Rahe
 *  E-mail: hquerquadrat@gmail.com
 *
 *  Based on gwyiew.c by David Necas
 *  www.gwyddion.net
 *
 *  Exports all the channels of SPM files readable by gwyddion to images
 *  and dumped metadata to text file
 *
 */

#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gprintf.h>

#include <libgwymodule/gwymodule.h>
#include <libgwymodule/gwymoduleenums.h>
#include <libgwymodule/gwymodule-process.h>
#include <libgwyddion/gwyddion.h>
#include <libgwydgets/gwydgets.h>
#include <libprocess/gwyprocess.h>
#include <libdraw/gwydraw.h>
#include <libdraw/gwygradient.h>
#include <app/gwyapp.h>
#include <libgwyddion/gwycontainer.h>
#include <libdraw/gwypixfield.h>
#include <libgwydgets/gwylayer-basic.h>
#include <libgwyddion/gwymd5.h>

#include <config.h>

#ifdef __unix__
      #include <unistd.h>
      #define GC_SLEEP(t) usleep(t)
#elif __MSDOS__ || __WIN32__ || _MSC_VER
      #include <windows.h>
      #include <io.h>
      #define GC_SLEEP(t) Sleep(t)
#else
      #error "Target system undefined. Quit."
#endif

#define PACKAGENAME "gwyexport"

typedef enum{
    JPEG,
    PNG,
} FileFormat;

typedef enum {
    EXPORT_RUNMODE_HELP=1,
    EXPORT_RUNMODE_VERSION,
    EXPORT_RUNMODE_IMG,
    EXPORT_RUNMODE_ERROR,
} ExportModes;

typedef enum {
    CMAP_AUTO,
    CMAP_ADAPTIVE,
    CMAP_FULL,
} ExportGlobals;

#define EXPORT_DEFAULT_FILTERLIST "pc;melc;sr;melc;pc"
#define EXPORT_FILTER_DELIMITER ";"

typedef struct {
    /* GwyExport Instance data */
    gchar* inputfile;
    gchar* outpath;
    FileFormat format;
    gchar* filterlist;
    gchar* gradient;
    ExportModes runmode;
    gboolean printmetafile;
    gboolean silentmode;
    ExportGlobals colormapping;
    GPtrArray *filelist;

    gint *channel_ids;
    gint n_channels;
} ExportGlobalParameters;


typedef struct {
    /* Abstract image data */
    gchar* ident;
    gchar* cycle_ident;
    gchar* title;
    gchar* filename;
    gchar* metafilename;
    gchar* date;
    gchar* scalebar_text;
    gdouble scalebar_relwidth;
    gchar* processing;

    /* Numerical values from image */
    gdouble colormin;
    gdouble colormax;

} ExportImageParameters;

/* String length for metadata keys */
#define STRN 100

#define GC_MESSAGE(gp, s, ...) \
    if (!gp->silentmode) { \
        g_message(s, ##__VA_ARGS__); \
    }
#define GC_WARNING(gp, s, ...) \
    if (!gp->silentmode) { \
        g_warning(s, ##__VA_ARGS__); \
    }
#define STR_APPEND(s, a, t) \
    if ( !s ) s = g_strdup(a); \
    else { \
        t = g_strconcat(s, g_strdup(", "), a, NULL); \
        g_free(s); \
        s = t; \
    } \

static void     handle_single_channel  (ExportGlobalParameters *gp,
                                        gint channelid);
static void     print_help             (void);
static void     init_gwyddion          (void);
static gboolean run_filters            (GwyContainer *datacont,
                                        GwyContainer *settings,
                                        ExportGlobalParameters *gp,
                                        ExportImageParameters *ip);
static void     process_args           (int argc, char* argv[],
                                        ExportGlobalParameters *gp);
static gboolean execute_process_module (gchar *procname,
                                        GwyContainer *data);
static ExportGlobalParameters* glob_params_new();
static ExportImageParameters*  img_params_new();
static gchar* scalebar_auto_length     (gdouble real,
                                        GwySIUnit *siunit,
                                        gdouble *p);
double pow10 ( double x );

/* The data file contents */
static GwyContainer *data;
/* And the Gwyddion settings */
static GwyContainer *settings;


/* Initialize gwyddion */
static void init_gwyddion(void)
{
    gwy_app_init_common(NULL, "layer", "file", "process", NULL);
    settings = gwy_app_settings_get();

    /* Disable undo function to save memory */
    gwy_undo_set_enabled(FALSE);
    gwy_app_data_browser_set_gui_enabled(FALSE);
}


/* Parses the command line arguments. */
static void
process_args(int argc, char* argv[], ExportGlobalParameters* gp)
{
    gint i=1;
    gint j=0;

    while (i < argc) {
        if (gwy_strequal(argv[i], "--help") ||
            gwy_strequal(argv[i], "-h")) {
            gp->runmode = EXPORT_RUNMODE_HELP;
            return;
        }
        if (gwy_strequal(argv[i], "--version") ||
            gwy_strequal(argv[i], "-v")) {
            gp->runmode = EXPORT_RUNMODE_VERSION;
            return;
        }

        if (gwy_strequal(argv[i], "--output") ||
            gwy_strequal(argv[i], "-o")) {
            if (i+1 < argc ) {
                gp->outpath = g_strdup(argv[++i]);
            } else {
                GC_WARNING(gp, "No output path defined\n");
            }
        }
        else if (gwy_strequal(argv[i], "--metadata") ||
                 gwy_strequal(argv[i], "-m")) {
            gp->printmetafile = TRUE;
        }
        else if (gwy_strequal(argv[i], "--filters") ||
                 gwy_strequal(argv[i], "-fl")) {
            // Filter list if complete
            if( i+1 < argc) {
                gp->filterlist = g_strdup(argv[++i]);
            } else {
                GC_WARNING(gp, "No filter list defined, will use default list.\n");
                gp->filterlist = g_strdup(EXPORT_DEFAULT_FILTERLIST);
            }
        }
        else if (gwy_strequal(argv[i], "--defaultfilters")) {
            // Default filters are used
            gp->filterlist = g_strdup(EXPORT_DEFAULT_FILTERLIST);
        }
        else if (gwy_strequal(argv[i], "--format") ||
                 gwy_strequal(argv[i], "-f")) {
            if(i+1<argc){
                ++i;
                if (gwy_strequal(argv[i], "png")) {
                    gp->format = PNG;
                }
                else if (gwy_strequal(argv[i], "jpg")) {
                    gp->format = JPEG;
                }
                else{
                    GC_WARNING(gp, "Unknow file format\n");
                }
            }
            else{
                GC_WARNING(gp, "File format missing");
            }
        }
        else if (gwy_strequal(argv[i], "--gradient") ||
                 gwy_strequal(argv[i], "-g")) {
            // Color gradient if complete
            if(i+1 < argc) {
                gp->gradient = g_strdup(argv[++i]);
            } else {
                GC_WARNING(gp, "No gradient defined\n");
            }
        }
        else if (gwy_strequal(argv[i], "--colormap") ||
                 gwy_strequal(argv[i], "-c")) {
            // Colormapping if complete
            if ( i+1 < argc ) {
                ++i;
                if (gwy_strequal(argv[i], "auto") ) {
                    gp->colormapping = CMAP_AUTO;
                } else if (gwy_strequal(argv[i], "full") ) {
                    gp->colormapping = CMAP_FULL;
                } else if (gwy_strequal(argv[i], "adaptive") ) {
                    gp->colormapping = CMAP_ADAPTIVE;
                } else {
                    GC_WARNING(gp, "Unknown colormapping `%s'. "
                                   "Using `adaptive'.", argv[i]);
                    gp->colormapping = CMAP_ADAPTIVE;
                }
            }
        }
        else if (gwy_strequal(argv[i], "--silentmode") ||
                 gwy_strequal(argv[i], "-s")) {
            gp->silentmode = TRUE;
        }
        else
        {
            /* We assume that all the following parameters are filenames */
            gp->runmode = EXPORT_RUNMODE_IMG;
            for(j=i;j<argc;++j){
                g_ptr_array_add(gp->filelist, g_strdup(argv[j]));
            }
            break;
        }
        ++i;
    }

    // Check argument for consistency
    if(gp->filelist->len == 0){
        GC_WARNING(gp, "No file given.\n");
        gp->runmode = EXPORT_RUNMODE_HELP;
        return;
    }
    if(!gp->outpath || gp->outpath == NULL) {
        // outpath undefined. Use the current directory
        gp->outpath = g_get_current_dir();
        GC_WARNING(gp, "No output path defined. Using directory:\n%s",
                   gp->outpath);
    }
    if(!gp->gradient || gp->gradient == NULL) {
        gp->gradient = "ReiGreen";
        GC_WARNING(gp, "No Gradient given. Using `ReiGreen' or default.");
    }
    if((!gp->colormapping) || (gp->colormapping <= 0)) {
        gp->colormapping = CMAP_AUTO;
        GC_WARNING(gp, "No Colormapping defined. Using `AUTO'.");
    }
    if(!gp->filterlist || gp->filterlist == NULL) {
        gp->filterlist = g_strdup(EXPORT_DEFAULT_FILTERLIST);
        GC_WARNING(gp, "No filters defined. Using defaults.");
    }

    return;
}

static ExportGlobalParameters* glob_params_new() {
    ExportGlobalParameters *gp=NULL;
    ExportGlobalParameters null = {0};

    gp = g_new(ExportGlobalParameters, 1);
    *gp = null;

    gp->silentmode = FALSE;
    gp->filelist = g_ptr_array_new();
    return gp;
}

static ExportImageParameters* img_params_new() {
    ExportImageParameters *ip=NULL;
    ExportImageParameters null = {0};

    ip = g_new(ExportImageParameters, 1);
    *ip = null;

    return ip;
}

static void handle_single_file(ExportGlobalParameters* gp, gchar* filename)
{
    GError *err = NULL;
    gint i=0;
    gp->inputfile = filename;

    /* Load the file */
    data = gwy_file_load(filename, GWY_RUN_NONINTERACTIVE, &err);
    if (!data) {
        GC_WARNING(gp, "Cannot load `%s': %s\n",
                   filename, err->message);
        g_clear_error(&err);
        return;
    }

    /* Register data to the data browser to be able to use
     * gwy_app_data_browser_get_data_ids() */
    gwy_app_data_browser_add(data);
    /* But do not let it manage our file */
    gwy_app_data_browser_set_keep_invisible(data, TRUE);

    /* Obtain the list of channel numbers and check whether
       there are any */
    gp->channel_ids = gwy_app_data_browser_get_data_ids(data);
    for (gp->n_channels = 0;
         gp->channel_ids[gp->n_channels] != -1; gp->n_channels++)
        ;
    if (gp->n_channels <= 0) {
        GC_WARNING(gp, "File `%s' contains no channels to export\n",
                   filename);
    }

    /* Iterate all channels */
    for (i = 0; i < gp->n_channels; ++i) {
      handle_single_channel(gp, i);
    }

    gwy_app_data_browser_remove(data);
    gwy_app_data_browser_shut_down();
    g_object_unref(data);
    g_free(err);
}

int
main(int argc, char *argv[])
{
    ExportGlobalParameters *gp=NULL;
    gp = glob_params_new();

    process_args(argc, argv, gp);

    /* Handle some run modes */
    if (gp->runmode == EXPORT_RUNMODE_HELP) {
        print_help();
        exit(0);
    }
    if (gp->runmode == EXPORT_RUNMODE_VERSION) {
        g_printf("%s %s\n", PACKAGENAME, VERSION);
        exit(0);
    }
    if (gp->runmode == EXPORT_RUNMODE_ERROR) {
        exit(1);
    }
    if(!gp->silentmode) {
      g_printf("==\nThis is %s v%s(2011) by François Bianco"
                   "(francois.bianco@unige.ch)\nBased on code by Philipp Rahe\n==\n", PACKAGENAME, VERSION);
    }

    gint i;
    const gchar* filename = NULL;
    const gchar* directory_path = NULL;
    for(i=0;i<gp->filelist->len;++i){
         /* Initialize Gtk+ */
        gtk_init(&argc, &argv);
        g_set_application_name(PACKAGENAME);

         /* Initialize Gwyddion stuff */
        init_gwyddion();
    
        filename = (gchar*) g_ptr_array_index(gp->filelist,i);
        if (g_file_test(filename, G_FILE_TEST_IS_DIR)) {
            directory_path = filename;
            GError *error = NULL;
            GDir* dir = g_dir_open(directory_path, 0, &error);
            if(error) {
                g_warning("g_dir_open() failed: %s\n", error->message);
                g_clear_error(&error);
                return 1;
            }
            while ((filename = g_dir_read_name(dir))){
                GC_MESSAGE(gp, "===> Processing file %s", filename);
                handle_single_file(gp, g_build_filename(directory_path,
                                                        filename, NULL));
            }
            g_dir_close(dir);
        }
        else if (g_file_test(filename, G_FILE_TEST_EXISTS)) {
            GC_MESSAGE(gp, "===> Processing file %s",
                       (gchar*) g_ptr_array_index(gp->filelist,i));
            handle_single_file(gp, (gchar*) g_ptr_array_index(gp->filelist,i));
        }

//      gwy_app_quit ();
        gtk_main_quit();

    }

    g_ptr_array_free(gp->filelist, TRUE);
    g_free(gp);

    return 0;
}

/* keys for the polylevel parameters */
static const gchar col_degree_key[]  = "/module/polylevel/col_degree";
static const gchar row_degree_key[]  = "/module/polylevel/row_degree";
static const gchar max_degree_key[]  = "/module/polylevel/max_degree";
static const gchar do_extract_key[]  = "/module/polylevel/do_extract";
static const gchar same_degree_key[] = "/module/polylevel/same_degree";
static const gchar independent_key[] = "/module/polylevel/independent";
static const gchar masking_key[]     = "/module/polylevel/masking";

/** Executes a process module with GWY_RUN_IMMEDIATE
 *  on the current channel in GwyContainer data
 */
static gboolean execute_process_module(gchar *procname,
                                       GwyContainer *data) {
    if (gwy_process_func_exists(procname)) {
        gwy_process_func_run(procname,
                             data, GWY_RUN_IMMEDIATE);
        return TRUE;
    } else {
        //GC_WARNING(gp, "processfunction `%s'"
        //           " is not available. Ignoring.\n", procname);
        return FALSE;
    }
    // Never reached
    return FALSE;
}


/** Applies the designated filters on the
 *  current GwyData
 */
static gboolean run_filters(GwyContainer *datacont,
                            GwyContainer *settings,
                            ExportGlobalParameters *gp,
                            ExportImageParameters *ip) {
    gboolean r = TRUE;
    gchar** filters = NULL;
    gchar* thisfilter = NULL;
    gint i=0, len=0;
    gchar *ptra=NULL, *ptrb=NULL;
    gint a=0, b=0;
    gboolean c = FALSE;
    gchar *temp=NULL;

    if(!gp->filterlist) {
        GC_WARNING(gp,
                   "No filterlist given. No filters will be used.");
        return FALSE;
    }
    filters = g_strsplit(gp->filterlist, EXPORT_FILTER_DELIMITER, 0);
    if (!filters) {
        GC_WARNING(gp,
                   "`%s' is no valid filterlist, using defaults.",
                  gp->filterlist);
        filters = g_strsplit(EXPORT_DEFAULT_FILTERLIST,
                             EXPORT_FILTER_DELIMITER, 0);
        if (!filters) {
            GC_WARNING(gp, "No valid default filterlist `%s'. "
                       "No filters will be applied",
                       EXPORT_DEFAULT_FILTERLIST);
            return FALSE;
        }
    }
    while ( (thisfilter = filters[i++]) != NULL) {
        ptra = NULL;
        ptrb = NULL;
        a = 0;
        b = 0;
        len = 0;
        c = FALSE;

        if (gwy_strequal(thisfilter, "pc") ) {
            /* Plane correct */
            r &= execute_process_module("level", datacont);
            STR_APPEND(ip->processing, "Plane level", temp);
        } else if (gwy_strequal(thisfilter, "melc")) {
            /* Median line correct */
            r &= execute_process_module("line_correct_median",datacont);
            STR_APPEND(ip->processing, "Median line correct", temp);
        } else if (gwy_strequal(thisfilter, "sr")) {
            /* Remove Scars */
            r &= execute_process_module("scars_remove", datacont);
            STR_APPEND(ip->processing, "Scars remove", temp);
        } else if (g_str_has_prefix(thisfilter, "poly")) {
            /* Polylevel */
            ptra = g_strrstr(thisfilter, ":");
            ptrb = g_strrstr(ptra, ",");
            if (! (ptra && ptrb)) {
                GC_WARNING(gp, "Illegal poly-filter: `%s'. Ignoring.",
                          thisfilter);
                r = FALSE;
                continue;
            }
            len = strlen(thisfilter);
            if( (thisfilter-ptra+1 < len) ) {
                a = atoi(ptra+1);
            }
            if( (thisfilter-ptrb+1 < len) ) {
                b = atoi(ptrb+1);
            } else {
                b = -1;
            }
            if ( (a > 0) && (b < 0) ) {
                b = a;
            }
            if ( (a < 0) || (b < 0) ) {
                GC_WARNING(gp, "Illegal poly grades: `%s'. Ignoring.",
                          thisfilter);
                r = FALSE;
                continue;
            }
            gwy_container_set_int32_by_name(settings,
                                            col_degree_key, a);
            gwy_container_set_int32_by_name(settings,
                                            row_degree_key, b);
            gwy_container_set_int32_by_name(settings,
                                            max_degree_key, 12);
            gwy_container_set_enum_by_name (settings,
                                            masking_key,
                                            GWY_MASK_IGNORE);
            gwy_container_set_boolean_by_name(settings,
                                              do_extract_key, FALSE);
            gwy_container_set_boolean_by_name(settings,
                                              same_degree_key, FALSE);
            gwy_container_set_boolean_by_name(settings,
                                              independent_key, TRUE);
            r &= execute_process_module("polylevel", datacont);
            STR_APPEND(ip->processing,
                       g_strdup_printf("Polynomial level: (%i,%i)",
                                       a, b), temp);

        } else if (g_str_has_prefix(thisfilter, "mean")) {
            /* Mean Filter */
            ptra = g_strrstr(thisfilter, ":");
            if(!ptra) {
                GC_WARNING(gp, "Illegal mean-filter: `%s'. Ignoring.",
                          thisfilter);
                r = FALSE;
                continue;
            }
            len = strlen(thisfilter);
            if (thisfilter-ptra+1 < len) {
                a = atoi(ptra+1);
            } else {
                GC_WARNING(gp, "Illegal mean-filter value: `%s'. "
                           "Ignoring.", thisfilter);
                r = FALSE;
                continue;
            }
            if(a <= 0) {
                GC_WARNING(gp, "Illegal mean value: `%i'. Ignoring.",
                           a);
                r = FALSE;
                continue;
            }
            GwyDataField *dfield;
            gwy_app_data_browser_get_current(GWY_APP_DATA_FIELD,
                                             &dfield, NULL);
            gwy_data_field_filter_mean(dfield, a);
            STR_APPEND(ip->processing,
                       g_strdup_printf("Mean filer: (%i pixel)",
                                       a), temp);
        } else if (g_str_has_prefix(thisfilter, "any")) {
            /* Execute the given process module */
            ptra = g_strrstr(thisfilter, ":");
            if(!ptra) {
                GC_WARNING(gp, "Illegal any-filter: `%s'. Ignoring.",
                          thisfilter);
                r = FALSE;
                continue;
            }
            len = strlen(thisfilter);
            if (thisfilter-ptra+1 < len) {
                c = execute_process_module(ptra+1, datacont);
                if (!c) {
                    GC_WARNING(gp, "Module `%s' could not be executed.",
                              ptra+1);
                    r = FALSE;
                } else {
                    STR_APPEND(ip->processing, ptra+1, temp);
                }
            } else {
                GC_WARNING(gp, "Illegal any-filter definition: `%s'.",
                          thisfilter);
            }
        } else if ( gwy_strequal(thisfilter, "") ) {
            /* Empty filter, ignoring. */
        } else {
            GC_WARNING(gp, "runfilters: Unknown filter `%s', ignoring.",
                      thisfilter);
        }
    }
    g_strfreev(filters);
    return r;
}


static void save_metadata(ExportGlobalParameters* gp, ExportImageParameters* iparams, gint ci)
{
    gint i;

    gchar tmetakey[STRN];
    GwyContainer *meta=NULL;
    g_snprintf(tmetakey, STRN, "/%i/meta", ci);
    if (! (gwy_container_contains_by_name(data, tmetakey) &&
        (meta = (GwyContainer*)gwy_container_get_object_by_name(
                                            data, tmetakey))) ) {
        GC_MESSAGE(gp, "Could not find a channel specific meta container, fall back on channel 0.");
    }

    g_snprintf(tmetakey, STRN, "/0/meta");
    if (! (gwy_container_contains_by_name(data, tmetakey) &&
        (meta = (GwyContainer*)gwy_container_get_object_by_name(
                                            data, tmetakey))) ) {
        GC_WARNING(gp, "Could not find any meta container, no metadata will be dumped.");
    } else
    {
        FILE * fp;
        GPtrArray *gparray = NULL;

        fp = fopen (iparams->metafilename,"w");
        fprintf(fp, "\"Info:Metadata\" string \"Dumped by %s v%s\"\n",
                                        PACKAGENAME, VERSION);
        fprintf(fp, "\"Info:Sourcefile\" string \"%s\"\n", gp->inputfile);
        gparray = gwy_container_serialize_to_text(meta);

        for(i=0; i<gparray->len; ++i) {
            fprintf(fp, "%s\n", (gchar*) g_ptr_array_index(gparray, i) );
        }
        g_ptr_array_free (gparray, TRUE);

        /* Also save the proccessing filters applied */
        fprintf(fp, "\"Info:Processing\" string \"%s\"\n", iparams->processing);

        fclose (fp);
    }

}

static void
handle_single_channel(ExportGlobalParameters* gp, gint ci)
{
    GtkWidget *view;
    GwyPixmapLayer *layer;
    GQuark quark;
    gchar *key, *basename, *newfilename, *ext, *basepath;
    GwyDataField *dfield;
    GdkPixbuf *pixbuf;
    GwyGradient *gradient;
    gint xres=0, yres=0;
    gboolean ok;
    ExportImageParameters *iparams;
    gchar *temp=NULL;

    iparams = img_params_new();

    g_return_if_fail( ci < gp->n_channels );

    /* Data view */
    view = gwy_data_view_new(data);
    /* The basic data display layer, constructed if called the
       first time. Each GwyDataView can hold several
       GwyDataViewLayers */
    layer = gwy_layer_basic_new();

    /* Set up the locations to display */
    quark = gwy_app_get_data_key_for_id(gp->channel_ids[ci]);
    gwy_data_view_set_data_prefix(GWY_DATA_VIEW(view),
                                  g_quark_to_string(quark));
    gwy_pixmap_layer_set_data_key(layer, g_quark_to_string(quark));
    gwy_data_view_set_base_layer(GWY_DATA_VIEW(view), layer);

    /* There is no helper function for palette keys, set it
       up manually */
    key = g_strdup_printf("/gwyexport/gradient");
    if(gp->gradient && (gp->gradient != NULL)) {
      gwy_layer_basic_set_gradient_key(GWY_LAYER_BASIC(layer), key);
      g_free(key);
      STR_APPEND(iparams->processing,
                 g_strdup_printf("Color gradient: `%s'", gp->gradient),
                 temp);
    }
    if (gp->colormapping && (gp->colormapping > 0)) {
        key = g_strdup_printf("/gwyexport/rangetype");
        if (gp->colormapping == CMAP_FULL) {
            gwy_container_set_int32_by_name(data, key,
                                      GWY_LAYER_BASIC_RANGE_FULL);
            gwy_layer_basic_set_range_type_key(GWY_LAYER_BASIC(layer),
                                               key);
            STR_APPEND(iparams->processing, "Color Range: Full", temp);
        } else if (gp->colormapping == CMAP_AUTO) {
            gwy_container_set_int32_by_name(data, key,
                                      GWY_LAYER_BASIC_RANGE_AUTO);
            gwy_layer_basic_set_range_type_key(GWY_LAYER_BASIC(layer),
                                               key);
            STR_APPEND(iparams->processing, "Color Range: Auto", temp);
        } else if (gp->colormapping == CMAP_ADAPTIVE) {
            gwy_container_set_int32_by_name(data, key,
                                      GWY_LAYER_BASIC_RANGE_ADAPT);
            gwy_layer_basic_set_range_type_key(GWY_LAYER_BASIC(layer),
                                               key);
            STR_APPEND(iparams->processing,
                       "Color Range: Adaptive", temp);
        }
        g_free(key);
    }

    /* Select the designated data field */
    gwy_app_data_browser_select_data_field(data, gp->channel_ids[ci]);
    gwy_app_data_browser_get_current(GWY_APP_DATA_FIELD, &dfield, NULL);
    iparams->title = gwy_app_get_data_field_title(data, gp->channel_ids[ci]);
    g_strdelimit(iparams->title, g_strdup(" "), '_');
    GC_MESSAGE(gp, "Processing channel %i : %s", gp->channel_ids[ci], iparams->title);

    /* Process the data */
    run_filters(data, settings, gp, iparams);

    /* Get the colorscale. This works, as `layer' and `pixbuf'
       have to do the same */
    gwy_layer_basic_get_range(GWY_LAYER_BASIC(layer),
                              &(iparams->colormin),
                              &(iparams->colormax));
    iparams->scalebar_text = scalebar_auto_length(
                                gwy_data_field_get_xreal(dfield),
                                gwy_data_field_get_si_unit_xy(dfield),
                                &iparams->scalebar_relwidth);

    /* Create the pixbuffer */
    if (!gp->gradient) gp->gradient = g_strdup("");
    gradient = gwy_gradients_get_gradient(gp->gradient);
    gwy_resource_use(GWY_RESOURCE(gradient));
    xres = gwy_data_field_get_xres(dfield);
    yres = gwy_data_field_get_yres(dfield);
    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, xres, yres);
    if (gp->colormapping == CMAP_AUTO) {
        gwy_pixbuf_draw_data_field_with_range(pixbuf, dfield, gradient,
                                              iparams->colormin,
                                              iparams->colormax);
    } else if (gp->colormapping == CMAP_FULL) {
        gwy_pixbuf_draw_data_field(pixbuf, dfield, gradient);
    } else if (gp->colormapping == CMAP_ADAPTIVE) {
        gwy_pixbuf_draw_data_field_adaptive(pixbuf, dfield, gradient);
    } else {
        GC_MESSAGE(gp, "No color mapping defined. Using adaptive.");
        gwy_pixbuf_draw_data_field_adaptive(pixbuf, dfield, gradient);
        STR_APPEND(iparams->processing, "Color Range: Adaptive", temp);
    }

    g_object_unref(dfield);
    gwy_resource_release(GWY_RESOURCE(gradient));

    /* Construct filename, path, ident and title  */
    if(!gp->outpath) gp->outpath = g_get_current_dir();

    basename = g_path_get_basename(gp->inputfile);
    newfilename = g_strdup_printf("%s-%i-%s", basename, ci, iparams->title);
    basepath = g_build_filename(gp->outpath, newfilename, NULL);
    ext = g_strdup(".txt");
    iparams->metafilename = g_strconcat(basepath, ext, NULL);
    g_free(ext);

    /* Save the GdkPixBuf to an image file */
    switch(gp->format){
        case PNG:
            ext = g_strdup_printf(".png");
            iparams->filename = g_strconcat(basepath, ext, NULL);
            ok = gdk_pixbuf_save(pixbuf, iparams->filename,
                            "png", NULL,
                            "compression", "9", NULL);
            g_free(ext);
        break;
        /* set jpeg as default */
        case JPEG:
        default:
            ext = g_strdup(".jpg");
            iparams->filename = g_strconcat(basepath, ext, NULL);
            ok = gdk_pixbuf_save(pixbuf, iparams->filename,
                                 "jpeg", NULL,
                                 "quality", "90", NULL);
            g_free(ext);
        break;
    }
    if(!gp->silentmode && ok) {
        GC_MESSAGE(gp, " => Saved to file `%s'", iparams->filename);
    }
    else if (!ok){
        GC_WARNING(gp, " Error file `%s' not saved", iparams->filename);
    }

    if(gp->printmetafile) {
        save_metadata(gp, iparams, ci);
    }

    g_free(iparams);
    g_object_unref(pixbuf);
    gtk_widget_destroy(view);
    g_free(basepath);
    g_free(newfilename);
    g_free(basename);

}

/** The following function is from modules/file/pixmap.c
 *  Gwyddion 2.19 by David Necas et al.
 */
static gchar*
scalebar_auto_length(gdouble real,
                     GwySIUnit *siunit,
                     gdouble *p)
{
    static const double sizes[] = {
        1.0, 2.0, 3.0, 4.0, 5.0,
        10.0, 20.0, 30.0, 40.0, 50.0,
        100.0, 200.0, 300.0, 400.0, 500.0,
    };
    GwySIValueFormat *format;
    gdouble base, x, vmax;
    gchar *s;
    gint power10;
    guint i;

    vmax = 0.42*real;
    power10 = 3*(gint)(floor(log10(vmax)/3.0));
    base = pow10(power10 + 1e-14);
    x = vmax/base;
    for (i = 1; i < G_N_ELEMENTS(sizes); i++) {
        if (x < sizes[i])
            break;
    }
    x = sizes[i-1] * base;

    format = gwy_si_unit_get_format_for_power10(siunit,
                                          GWY_SI_UNIT_FORMAT_VFMARKUP,
                                          power10, NULL);
    s = g_strdup_printf("%.*f %s",
                        format->precision, x/format->magnitude,
                        format->units);
    gwy_si_unit_value_format_free(format);

    if (p)
        *p = x/real;

    return s;
}


/* Print help */
static void
print_help(void)
{
    g_print(
"Usage: %s -o <output-path> [Options] <filenames>\n\n"
"Exports any readable SPM data file to png or jpg images.\n"
"Uses the Gwyddion libraries for fileopening and processing.\n"
"If --metadata is specified, additional information is written to a \
text file. \n\n",
    PACKAGENAME
        );
    g_print(
"Options:\n"
" -h, --help                  Print this help and terminate.\n"
" -v, --version               Print version info and terminate.\n"
" -s, --silentmode            Only filenames of created images printed.\n"
" -o, --outpath <output-path> The path, where the exported files are saved.\n"
"                             If no path is specified images will be stored in\n"
"                             the current directory.\n"
" -f, --format <format>       The export format either 'jpg' or 'png'.\n"
" -m, --metadata              Will dump the metadata into a text file for each\n"
"                             channel. The metadata file will have the same\n"
"                             name and outpath as the image file.\n"
" -fl, --filters <filters>    Specifies filters applied to each image.\n"
"                             <filters> is a list, separated by `%s'.\n",
    EXPORT_FILTER_DELIMITER);
    g_printf(
"                             Filters are processed in given order. \n"
"                             Filter can be:\n\n"
"                               pc        - Plane correct.\n"
"                               melc      - Median line correction.\n"
"                               sr        - Remove scars.\n"
"                               poly:x,y  - Polylevel with degrees x,y.\n"
"                               mean:x    - Mean filter of x pixel.\n"
"                               any:name  - Process module <name> \n"
"                                           will be executed.\n"
"                             Example: -filters pc%smelc%spoly:2,2%smelc\n",
    EXPORT_FILTER_DELIMITER, EXPORT_FILTER_DELIMITER, EXPORT_FILTER_DELIMITER
    );
    g_print(
" --defaultfilters            Uses a predefined filterlist.\n"
"                             Same as `--filters %s'\n",
    EXPORT_DEFAULT_FILTERLIST);
    g_print(
" -g, --gradient <gradient>   Name of the colorgradient to be used.\n"
"                             If no gradient given, the gwyddion-default\n"
"                             will be used.\n"
" -c, --colormap <map>        Can be: [auto|full|adaptive] for the \n"
"                             respective mapping to colors. Default is \n"
"                             `adaptive'.\n\n"
                                        );
    g_print("Report bugs to <francois.bianco@unige.ch>\n");

}

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */

