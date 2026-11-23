#include <gtk/gtk.h>
#include <stdlib.h>

#define APP_ID "io.github.MihailKirzhakov.dirtree"

#define UNUSED __attribute__((unused))

#define FREE_AND_NULL(p) \
    do {                 \
        g_free(p);       \
        (p) = NULL;      \
    } while (0)

#define QUERY_ATTRIBUTES                         \
    G_FILE_ATTRIBUTE_STANDARD_NAME ","           \
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","   \
    G_FILE_ATTRIBUTE_STANDARD_TYPE ","           \
    G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK ","     \
    G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET "," \
    G_FILE_ATTRIBUTE_STANDARD_SIZE ","           \
    G_FILE_ATTRIBUTE_STANDARD_SYMBOLIC_ICON

enum {
    COL_ICON,
    COL_NAME,
    COL_KIND,
    COL_SIZE_STR,
    COL_SIZE,
    COL_IS_DIR,
    N_COLUMNS
};

typedef struct {
    guint dirs;
    guint files;
    guint errors;
} ScanStats;

typedef struct {
    int exit_status;
} AppState;

static const gchar *
file_kind(GFileInfo *info)
{
    const gboolean is_link = g_file_info_get_is_symlink(info);

    switch (g_file_info_get_file_type(info)) {
    case G_FILE_TYPE_DIRECTORY:
        return is_link ? "Ссылка на каталог" : "Каталог";
    case G_FILE_TYPE_REGULAR:
        return is_link ? "Ссылка на файл" : "Файл";
    case G_FILE_TYPE_SYMBOLIC_LINK:
        return "Битая ссылка";
    case G_FILE_TYPE_SPECIAL:
        return "Специальный файл";
    case G_FILE_TYPE_SHORTCUT:
    case G_FILE_TYPE_MOUNTABLE:
    case G_FILE_TYPE_UNKNOWN:
    default:
        return "Неизвестно";
    }
}

static void
warn_file_error(GFile *file, const GError *error)
{
    gchar *name = g_file_get_parse_name(file);

    g_warning(
        "«%s»: %s",
        name != NULL ? name : "?",
        error != NULL ? error->message : "неизвестная ошибка"
    );
    FREE_AND_NULL(name);
}

static gboolean
resolve_link_type(GFile *file, GFileInfo *info, GError **error)
{
    GFileInfo *target = g_file_query_info(
        file,
        G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NONE,
        NULL,
        error
    );
    if (target == NULL) {
        return FALSE;
    }

    g_file_info_set_file_type(info, g_file_info_get_file_type(target));
    g_clear_object(&target);
    return TRUE;
}

static gchar *
make_display_name(GFileInfo *info)
{
    const gchar *display_name = g_file_info_get_display_name(info);
    const gchar *link_target = NULL;

    if (display_name == NULL) {
        return NULL;
    }

    if (g_file_info_get_is_symlink(info)) {
        link_target = g_file_info_get_symlink_target(info);
    }

    if (link_target != NULL) {
        return g_strdup_printf("%s → %s", display_name, link_target);
    }
    return g_strdup(display_name);
}

static gboolean
add_entry(
    GtkTreeStore *store,
    GtkTreeIter *parent,
    GFile *file,
    GFileInfo *info,
    GtkTreeIter *out_iter,
    GError **error
)
{
    gchar *name = NULL;
    gchar *size_str = NULL;
    gboolean is_dir = FALSE;
    guint64 size = 0;

    if (g_file_info_get_is_symlink(info)) {
        GError *link_error = NULL;

        if (!resolve_link_type(file, info, &link_error)) {
            if (!g_error_matches(link_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
                warn_file_error(file, link_error);
            }
            g_clear_error(&link_error);
        }
    }

    is_dir = (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY);
    size = (guint64)g_file_info_get_size(info);

    name = make_display_name(info);
    if (name == NULL) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "Не удалось получить имя файла"
        );
        return FALSE;
    }

    size_str = is_dir ? g_strdup("") : g_format_size(size);
    if (size_str == NULL) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "Не удалось сформировать размер файла"
        );
        FREE_AND_NULL(name);
        return FALSE;
    }

    gtk_tree_store_insert_with_values(
        store, out_iter, parent, -1,
        COL_ICON, g_file_info_get_symbolic_icon(info),
        COL_NAME, name,
        COL_KIND, file_kind(info),
        COL_SIZE_STR, size_str,
        COL_SIZE, size,
        COL_IS_DIR, is_dir,
        -1
    );

    FREE_AND_NULL(name);
    FREE_AND_NULL(size_str);
    return TRUE;
}

static gboolean
populate(
    GtkTreeStore *store,
    GtkTreeIter *parent,
    GFile *dir,
    ScanStats *stats,
    GError **error
)
{
    g_return_val_if_fail(GTK_IS_TREE_STORE(store), FALSE);
    g_return_val_if_fail(G_IS_FILE(dir), FALSE);
    g_return_val_if_fail(stats != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    gboolean ok = TRUE;
    GFileEnumerator *en = g_file_enumerate_children(
        dir,
        QUERY_ATTRIBUTES,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        NULL,
        error
    );

    if (en == NULL) {
        return FALSE;
    }

    for (;;) {
        GFileInfo *info = NULL;
        GFile *child = NULL;
        GtkTreeIter iter = {0};

        if (!g_file_enumerator_iterate(en, &info, &child, NULL, error)) {
            ok = FALSE;
            break;
        }
        if (info == NULL) {
            break;
        }

        if (!add_entry(store, parent, child, info, &iter, error)) {
            ok = FALSE;
            break;
        }

        if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
            stats->files++;
            continue;
        }

        stats->dirs++;
        if (!g_file_info_get_is_symlink(info)) {
            GError *child_error = NULL;

            if (!populate(store, &iter, child, stats, &child_error)) {
                warn_file_error(child, child_error);
                g_clear_error(&child_error);
                stats->errors++;
            }
        }
    }

    if (!g_file_enumerator_close(en, NULL, ok ? error : NULL)) {
        ok = FALSE;
    }
    g_clear_object(&en);
    return ok;
}

static gint
compare_by_name(
    GtkTreeModel *model,
    GtkTreeIter *a,
    GtkTreeIter *b,
    gpointer user_data UNUSED
)
{
    gboolean dir_a = FALSE;
    gboolean dir_b = FALSE;
    gchar *name_a = NULL;
    gchar *name_b = NULL;
    gint res = 0;

    gtk_tree_model_get(model, a, COL_IS_DIR, &dir_a, COL_NAME, &name_a, -1);
    gtk_tree_model_get(model, b, COL_IS_DIR, &dir_b, COL_NAME, &name_b, -1);

    if (dir_a != dir_b) {
        res = dir_a ? -1 : 1;
    } else if (name_a == NULL || name_b == NULL) {
        res = (name_a != NULL) - (name_b != NULL);
    } else {
        res = g_utf8_collate(name_a, name_b);
    }

    FREE_AND_NULL(name_a);
    FREE_AND_NULL(name_b);
    return res;
}

static gint
compare_by_size(
    GtkTreeModel *model,
    GtkTreeIter *a,
    GtkTreeIter *b,
    gpointer user_data
)
{
    guint64 size_a = 0;
    guint64 size_b = 0;

    gtk_tree_model_get(model, a, COL_SIZE, &size_a, -1);
    gtk_tree_model_get(model, b, COL_SIZE, &size_b, -1);

    if (size_a != size_b) {
        return size_a < size_b ? -1 : 1;
    }
    return compare_by_name(model, a, b, user_data);
}

static GtkTreeModel *
create_model(GFile *root, ScanStats *stats, GError **error)
{
    GtkTreeStore *store = gtk_tree_store_new(
        N_COLUMNS,
        G_TYPE_ICON,
        G_TYPE_STRING,
        G_TYPE_STRING,
        G_TYPE_STRING,
        G_TYPE_UINT64,
        G_TYPE_BOOLEAN
    );
    GtkTreeSortable *sortable = NULL;

    if (store == NULL) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "Не удалось создать модель дерева"
        );
        return NULL;
    }

    if (!populate(store, NULL, root, stats, error)) {
        g_clear_object(&store);
        return NULL;
    }

    sortable = GTK_TREE_SORTABLE(store);
    gtk_tree_sortable_set_sort_func(sortable, COL_NAME, compare_by_name, NULL, NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_SIZE, compare_by_size, NULL, NULL);
    gtk_tree_sortable_set_sort_column_id(sortable, COL_NAME, GTK_SORT_ASCENDING);

    return GTK_TREE_MODEL(store);
}

static void
discard_floating(gpointer object)
{
    if (object != NULL) {
        g_object_unref(g_object_ref_sink(object));
    }
}

static gboolean
append_name_column(GtkTreeView *view)
{
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *text = gtk_cell_renderer_text_new();

    if (col == NULL || icon == NULL || text == NULL) {
        discard_floating(col);
        discard_floating(icon);
        discard_floating(text);
        return FALSE;
    }

    gtk_tree_view_column_set_title(col, "Имя");
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_sort_column_id(col, COL_NAME);

    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "gicon", COL_ICON);
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", COL_NAME);

    gtk_tree_view_append_column(view, col);
    return TRUE;
}

static gboolean
append_text_column(
    GtkTreeView *view,
    const gchar *title,
    gint text_col,
    gint sort_col,
    gfloat xalign
)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = NULL;

    if (renderer == NULL) {
        return FALSE;
    }
    g_object_set(renderer, "xalign", xalign, NULL);

    col = gtk_tree_view_column_new_with_attributes(title, renderer, "text", text_col, NULL);
    if (col == NULL) {
        discard_floating(renderer);
        return FALSE;
    }

    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_alignment(col, xalign);
    gtk_tree_view_column_set_sort_column_id(col, sort_col);

    gtk_tree_view_append_column(view, col);
    return TRUE;
}

static GtkWidget *
create_view(GtkTreeModel *model)
{
    GtkWidget *view = gtk_tree_view_new_with_model(model);

    if (view == NULL) {
        return NULL;
    }

    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(view), COL_NAME);

    if (
        !append_name_column(GTK_TREE_VIEW(view))
        || !append_text_column(GTK_TREE_VIEW(view), "Тип", COL_KIND, COL_KIND, 0.0f)
        || !append_text_column(GTK_TREE_VIEW(view), "Размер", COL_SIZE_STR, COL_SIZE, 1.0f)
    ) {
        discard_floating(view);
        return NULL;
    }

    return view;
}

static GtkWidget *
create_window(
    GtkApplication *app,
    const gchar *title,
    GtkTreeModel *model,
    const ScanStats *stats
)
{
    GtkWidget *window = NULL;
    GtkWidget *box = NULL;
    GtkWidget *scrolled = NULL;
    GtkWidget *view = NULL;
    GtkWidget *status = NULL;
    gchar *summary = NULL;

    window = gtk_application_window_new(app);
    if (window == NULL) {
        return NULL;
    }
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (box == NULL) {
        goto fail;
    }
    gtk_container_add(GTK_CONTAINER(window), box);

    scrolled = gtk_scrolled_window_new(NULL, NULL);
    if (scrolled == NULL) {
        goto fail;
    }
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled),
        GTK_POLICY_AUTOMATIC,
        GTK_POLICY_AUTOMATIC
    );
    gtk_box_pack_start(GTK_BOX(box), scrolled, TRUE, TRUE, 0);

    view = create_view(model);
    if (view == NULL) {
        goto fail;
    }
    gtk_container_add(GTK_CONTAINER(scrolled), view);

    summary = g_strdup_printf(
        "Каталогов: %u, файлов: %u%s",
        stats->dirs,
        stats->files,
        stats->errors ? ", есть ошибки доступа (см. журнал)" : ""
    );
    if (summary == NULL) {
        goto fail;
    }

    status = gtk_label_new(summary);
    FREE_AND_NULL(summary);
    if (status == NULL) {
        goto fail;
    }
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status, 6);
    gtk_widget_set_margin_top(status, 3);
    gtk_widget_set_margin_bottom(status, 3);
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);

    return window;

fail:
    gtk_widget_destroy(window);
    return NULL;
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    AppState *state = user_data;
    GError *error = NULL;
    gchar *cwd = NULL;
    GFile *root = NULL;
    GtkTreeModel *model = NULL;
    GtkWidget *window = NULL;
    ScanStats stats = {0, 0, 0};

    cwd = g_get_current_dir();
    if (cwd == NULL) {
        g_printerr("Не удалось определить текущий каталог\n");
        goto fail;
    }

    root = g_file_new_for_path(cwd);
    if (root == NULL) {
        g_printerr("Не удалось открыть каталог «%s»\n", cwd);
        goto fail;
    }

    model = create_model(root, &stats, &error);
    if (model == NULL) {
        g_printerr(
            "Не удалось прочитать каталог «%s»: %s\n",
            cwd,
            error != NULL ? error->message : "неизвестная ошибка"
        );
        goto fail;
    }

    window = create_window(app, cwd, model, &stats);
    if (window == NULL) {
        g_printerr("Не удалось создать главное окно\n");
        goto fail;
    }

    gtk_widget_show_all(window);
    goto out;

fail:
    state->exit_status = EXIT_FAILURE;
out:
    g_clear_object(&model);
    g_clear_object(&root);
    g_clear_error(&error);
    FREE_AND_NULL(cwd);
}

int
main(int argc, char *argv[])
{
    AppState state = {EXIT_SUCCESS};
    GtkApplication *app = NULL;
    int status = EXIT_SUCCESS;

    app = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
    if (app == NULL) {
        g_printerr("Не удалось создать приложение\n");
        return EXIT_FAILURE;
    }

    if (g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state) == 0) {
        g_printerr("Не удалось подключить обработчик сигнала activate\n");
        g_clear_object(&app);
        return EXIT_FAILURE;
    }

    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_clear_object(&app);

    if (status == EXIT_SUCCESS) {
        status = state.exit_status;
    }
    return status;
}
