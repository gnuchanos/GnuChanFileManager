#!/usr/bin/env python3
import os
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except Exception as e:
    raise SystemExit(f"Pillow (PIL) gerekli: {e}")

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "icons" / "ui"
SIZE = 20  # icon content size; drawn centered in buttons

FG = (235, 235, 235, 255)
ACCENT = (170, 130, 255, 255)
MUTED = (180, 180, 180, 255)
BG = (0, 0, 0, 0)


def new_img(s=SIZE):
    return Image.new("RGBA", (s, s), BG)


def save(name, img):
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f"{name}.png"
    img.save(path)
    print("generated", path)


def icon_back():
    img = new_img(); d = ImageDraw.Draw(img)
    d.polygon([(14,4),(6,10),(14,16)], fill=FG)
    save("back", img)

def icon_forward():
    img = new_img(); d = ImageDraw.Draw(img)
    d.polygon([(6,4),(14,10),(6,16)], fill=FG)
    save("forward", img)

def icon_up():
    img = new_img(); d = ImageDraw.Draw(img)
    d.polygon([(10,4),(4,12),(16,12)], fill=FG)
    d.rectangle([9,12,11,17], fill=FG)
    save("up", img)

def icon_go():
    img = new_img(); d = ImageDraw.Draw(img)
    d.line([(4,10),(14,10)], fill=ACCENT, width=2)
    d.polygon([(12,6),(16,10),(12,14)], fill=ACCENT)
    save("go", img)

def icon_refresh():
    img = new_img(); d = ImageDraw.Draw(img)
    d.arc([4,4,16,16], start=40, end=320, fill=FG, width=2)
    d.polygon([(14,3),(18,6),(13,7)], fill=FG)
    save("refresh", img)

def icon_hidden():
    img = new_img(); d = ImageDraw.Draw(img)
    d.ellipse([3,7,17,13], outline=FG, width=2)
    d.ellipse([8,8,12,12], fill=FG)
    d.line([4,16,16,4], fill=ACCENT, width=2)
    save("hidden", img)

def icon_new_folder():
    img = new_img(); d = ImageDraw.Draw(img)
    d.rectangle([2,7,14,16], outline=FG, width=2)
    d.rectangle([2,5,8,7], fill=FG)
    d.line([15,9,15,15], fill=ACCENT, width=2)
    d.line([12,12,18,12], fill=ACCENT, width=2)
    save("new_folder", img)

def icon_new_file():
    img = new_img(); d = ImageDraw.Draw(img)
    d.polygon([(4,3),(12,3),(16,7),(16,17),(4,17)], outline=FG)
    d.line([12,3,12,7], fill=FG); d.line([12,7,16,7], fill=FG)
    d.line([10,10,10,15], fill=ACCENT, width=2)
    d.line([7,12,13,12], fill=ACCENT, width=2)
    save("new_file", img)

def icon_bm_add():
    img = new_img(); d = ImageDraw.Draw(img)
    d.polygon([(5,3),(15,3),(15,17),(10,14),(5,17)], outline=FG)
    d.line([10,6,10,11], fill=ACCENT, width=2)
    d.line([7,8,13,8], fill=ACCENT, width=2)
    save("bm_add", img)

def icon_bm_remove():
    img = new_img(); d = ImageDraw.Draw(img)
    d.polygon([(5,3),(15,3),(15,17),(10,14),(5,17)], outline=FG)
    d.line([7,8,13,8], fill=ACCENT, width=2)
    save("bm_remove", img)

def icon_view_list():
    img = new_img(); d = ImageDraw.Draw(img)
    for y in (5,9,13):
        d.rectangle([4,y,6,y+2], fill=FG)
        d.line([8,y+1,16,y+1], fill=FG, width=2)
    save("view_list", img)

def icon_view_icons():
    img = new_img(); d = ImageDraw.Draw(img)
    for x in (4,11):
        for y in (4,11):
            d.rectangle([x,y,x+4,y+4], outline=FG)
    save("view_icons", img)

def icon_view_large():
    img = new_img(); d = ImageDraw.Draw(img)
    d.rectangle([3,3,10,10], outline=FG)
    d.rectangle([10,10,17,17], outline=FG)
    d.line([5,14,15,6], fill=ACCENT, width=2)
    save("view_large", img)


def main():
    icon_back(); icon_forward(); icon_up(); icon_go(); icon_refresh()
    icon_hidden(); icon_new_folder(); icon_new_file()
    icon_bm_add(); icon_bm_remove()
    icon_view_list(); icon_view_icons(); icon_view_large()

if __name__ == '__main__':
    main()
