# Builds the .nro's home-menu icon out of the game's own artwork.
#
# The APK's launcher icon tops out at 144x144 and the Switch wants 256x256, so
# upscaling it would show. The menu background is 1024x512 and the Nord on its
# right-hand side is both the highest-resolution art in the game and the image
# players associate with Warband's title screen - cropped to his head and
# shoulders it reads at home-menu size, which a wide scene never does.
#
#   python tools/make_icon.py            # writes switch/icon.jpg
#   python tools/make_icon.py --preview  # also writes the crop as a .png
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
SOURCE = os.path.join(ROOT, "game", "gamedata", "Textures", "main_menu_nord.dds")
OUTPUT = os.path.join(ROOT, "switch", "icon.jpg")

# Head and shoulders, in the 1024x512 artwork's own coordinates. The head sits
# high in the picture, so the square cannot be centred on it without running
# off the top - which is how a portrait is framed anyway.
CROP = (620, 0, 940, 320)
SIZE = 256


def main():
    if not os.path.exists(SOURCE):
        print("cannot find %s - run setup.bat to extract the game data first"
              % SOURCE)
        return 1
    art = Image.open(SOURCE).convert("RGB")
    icon = art.crop(CROP).resize((SIZE, SIZE), Image.LANCZOS)
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    # elf2nro wants a JPEG; quality 92 keeps the mail and the beard from
    # turning to mush at a file size nobody will notice.
    icon.save(OUTPUT, "JPEG", quality=92, optimize=True)
    print("wrote %s (%dx%d, %d bytes)"
          % (OUTPUT, SIZE, SIZE, os.path.getsize(OUTPUT)))
    if "--preview" in sys.argv:
        preview = os.path.splitext(OUTPUT)[0] + "_preview.png"
        icon.save(preview)
        print("wrote", preview)
    return 0


sys.exit(main())
