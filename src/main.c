#include <gtk/gtk.h>

#define APP_ID "io.github.MihailKirzhakov.dirtree"

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
populate(GtkTreeStore *store, GtkTreeIter *parent, GFile *dir, ScanStats *stats)
{
    GError *error = NULL;
    GFileEnumerator *en = g_file_enumerate_children(
        dir, QUERY_ATTRIBUTES, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);

    if (en == NULL) {
        gchar *path = g_file_get_path(dir);
        g_warning("Не удалось прочитать каталог «%s»: %s", path, error->message);
        g_free(path);
        g_error_free(error);
        stats->errors++;
        return;
    }

    for (;;) {
        GFileInfo *info = NULL;
        GFile *child = NULL;

        if (!g_file_enumerator_iterate(en, &info, &child, NULL, &error)) {
            g_warning("Ошибка чтения каталога: %s", error->message);
            g_clear_error(&error);
            stats->errors++;
            break;
        }
        if (info == NULL)
            break;

        GFileType type = g_file_info_get_file_type(info);
        const gboolean is_link = g_file_info_get_is_symlink(info);
        if (is_link) {
            GFileInfo *target = g_file_query_info(
                child, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (target != NULL) {
                type = g_file_info_get_file_type(target);
                g_file_info_set_file_type(info, type);
                g_object_unref(target);
            }
        }

        const gboolean is_dir = (type == G_FILE_TYPE_DIRECTORY);
        const guint64 size = (guint64)g_file_info_get_size(info);
        gchar *size_str = is_dir ? g_strdup("") : g_format_size(size);
        gchar *name = NULL;

        if (is_link && g_file_info_get_symlink_target(info) != NULL)
            name = g_strdup_printf(
                "%s → %s",
                g_file_info_get_display_name(info),
                g_file_info_get_symlink_target(info)
            );
        else
            name = g_strdup(g_file_info_get_display_name(info));

        GtkTreeIter iter;
        gtk_tree_store_insert_with_values(
            store, &iter, parent, -1,
            COL_ICON, g_file_info_get_symbolic_icon(info),
            COL_NAME, name,
            COL_KIND, file_kind(info),
            COL_SIZE_STR, size_str,
            COL_SIZE, size,
            COL_IS_DIR, is_dir,
            -1
        );
        g_free(name);
        g_free(size_str);

        if (is_dir) {
            stats->dirs++;
            if (!is_link)
                populate(store, &iter, child, stats);
        } else {
            stats->files++;
        }
    }

    g_object_unref(en);
}

static gint
compare_by_name(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
    (void)user_data;

    gboolean dir_a, dir_b;
    gchar *name_a, *name_b;
    gtk_tree_model_get(model, a, COL_IS_DIR, &dir_a, COL_NAME, &name_a, -1);
    gtk_tree_model_get(model, b, COL_IS_DIR, &dir_b, COL_NAME, &name_b, -1);

    gint res;
    if (dir_a != dir_b)
        res = dir_a ? -1 : 1;
    else if (name_a == NULL || name_b == NULL)
        res = (name_a != NULL) - (name_b != NULL);
    else
        res = g_utf8_collate(name_a, name_b);

    g_free(name_a);
    g_free(name_b);
    return res;
}

static gint
compare_by_size(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
    guint64 size_a, size_b;
    gtk_tree_model_get(model, a, COL_SIZE, &size_a, -1);
    gtk_tree_model_get(model, b, COL_SIZE, &size_b, -1);

    if (size_a != size_b)
        return size_a < size_b ? -1 : 1;
    return compare_by_name(model, a, b, user_data);
}

static GtkTreeModel *
create_model(GFile *root, ScanStats *stats)
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

    populate(store, NULL, root, stats);

    GtkTreeSortable *sortable = GTK_TREE_SORTABLE(store);
    gtk_tree_sortable_set_sort_func(sortable, COL_NAME, compare_by_name, NULL, NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_SIZE, compare_by_size, NULL, NULL);
    gtk_tree_sortable_set_sort_column_id(sortable, COL_NAME, GTK_SORT_ASCENDING);

    return GTK_TREE_MODEL(store);
}

static GtkWidget *
create_view(GtkTreeModel *model)
{
    GtkWidget *view = gtk_tree_view_new_with_model(model);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(view), COL_NAME);

    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, "Имя");
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_sort_column_id(col, COL_NAME);

    GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, renderer, FALSE);
    gtk_tree_view_column_add_attribute(col, renderer, "gicon", COL_ICON);

    renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, renderer, TRUE);
    gtk_tree_view_column_add_attribute(col, renderer, "text", COL_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

    renderer = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Тип", renderer, "text", COL_KIND, NULL);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_sort_column_id(col, COL_KIND);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xalign", 1.0f, NULL);
    col = gtk_tree_view_column_new_with_attributes("Размер", renderer, "text", COL_SIZE_STR, NULL);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_alignment(col, 1.0f);
    gtk_tree_view_column_set_sort_column_id(col, COL_SIZE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

    return view;
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;

    gchar *cwd = g_get_current_dir();
    GFile *root = g_file_new_for_path(cwd);

    ScanStats stats = {0, 0, 0};
    GtkTreeModel *model = create_model(root, &stats);
    GtkWidget *view = create_view(model);
    g_object_unref(model);
    g_object_unref(root);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC
    );
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_container_add(GTK_CONTAINER(scrolled), view);

    gchar *summary = g_strdup_printf(
        "Каталогов: %u, файлов: %u%s",
        stats.dirs, stats.files,
        stats.errors ? ", есть ошибки доступа (см. журнал)" : ""
    );
    GtkWidget *status = gtk_label_new(summary);
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status, 6);
    gtk_widget_set_margin_top(status, 3);
    gtk_widget_set_margin_bottom(status, 3);
    g_free(summary);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), scrolled, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), cwd);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    gtk_container_add(GTK_CONTAINER(window), box);
    gtk_widget_show_all(window);

    g_free(cwd);
}

int
main(int argc, char *argv[])
{
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
