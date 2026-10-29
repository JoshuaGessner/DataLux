import os


application = os.path.abspath(defines["app"])  # noqa: F821
app_name = os.path.basename(application)
metadata = os.path.abspath(defines["metadata"])  # noqa: F821
background = os.path.abspath(defines["background"])  # noqa: F821
volume_icon = os.path.abspath(defines["volume_icon"])  # noqa: F821

format = "UDZO"
filesystem = "HFS+"
compression_level = 9

files = [
    (application, app_name),
    (metadata, ".fileflow"),
]
symlinks = {"Applications": "/Applications"}
hide = [".fileflow", ".VolumeIcon.icns"]
icon = volume_icon

# Finder's window height includes its title bar. Add 32 points so the 660×380
# background remains fully visible in the content area.
window_rect = ((160, 120), (660, 412))
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
show_icon_preview = True
include_icon_view_settings = True
include_list_view_settings = False

arrange_by = None
grid_spacing = 90
label_pos = "bottom"
text_size = 13
icon_size = 112
icon_locations = {
    app_name: (170, 220),
    "Applications": (490, 220),
}
