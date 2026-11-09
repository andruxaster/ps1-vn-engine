#!/usr/bin/env python3
"""Перенос простой игры Ren'Py в проект движка.

  python3 tools/import_renpy.py <путь к game/ игры Ren'Py> [--lang russian]

Делает три вещи:
  1. картинки: фоны 16:9 приводит к 320x240 (масштаб по высоте и обрезка
     по центру), спрайты масштабирует в той же пропорции, что и экран
     (720 -> 240), и раскладывает по art/bg и art/char/<кто>/<эмоция>.png;
  2. сценарий: переводит script.rpy в язык движка (script/main.vns),
     подставляя реплики из перевода tl/<lang>/script.rpy, если он есть;
  3. печатает, что перенести не удалось.

Это НЕ полноценный транслятор Ren'Py: Ren'Py — целый язык на Python, а
здесь разбирается только самое ходовое подмножество, которым написаны
простые новеллы, включая демо «The Question» из поставки Ren'Py:

    define x = Character(_("Имя"), ...)   имена говорящих
    default flag = False                  флаги (становятся set N 0)
    label / jump / return                 метки и переходы
    scene bg X [with ...]                 смена фона через затемнение
    scene black                           чёрный экран
    show who attr [attr] [with ...]       спрайт персонажа
    hide who                              убрать спрайт
    menu: с подписью и вариантами         выбор
    $ flag = True / False                 запись флага
    if flag:                              условный блок (одно условие)
    play music "..." / stop music         музыка (имя трека из --music)
    "текст" / who "текст"                 реплики

Всё прочее печатается как пропущенное, чтобы перенос не был молча
неполным.
"""

import argparse
import ast
import os
import re
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCR_W, SCR_H = 320, 240
SRC_H = 720                       # высота экрана Ren'Py по умолчанию

# Символы, которых нет в шрифте движка: заменяем на ближайшие, иначе они
# молча превратятся в пробелы.
GLYPH_FIX = {
    "…": "...", "—": "-", "–": "-", "«": "\"",
    "»": "\"", "“": "\"", "”": "\"", "’": "'",
    "‘": "'", " ": " ",
}


def clean_text(t):
    t = re.sub(r"\{/?[a-z]+[^}]*\}", "", t)          # теги {b}, {i}, ...
    t = t.replace('\\"', '"')
    for a, b in GLYPH_FIX.items():
        t = t.replace(a, b)
    return t.strip()


# ---------------------------------------------------------------- картинки

def convert_images(game, report):
    src = os.path.join(game, "images")
    sizes = {}
    for f in sorted(os.listdir(src)):
        name, ext = os.path.splitext(f)
        if ext.lower() not in (".png", ".jpg", ".jpeg", ".webp"):
            continue
        parts = name.split()
        im = Image.open(os.path.join(src, f))

        if parts[0] == "bg":
            im = im.convert("RGB")
            k = SCR_H / im.size[1]
            w = round(im.size[0] * k)
            im = im.resize((w, SCR_H), Image.LANCZOS)
            x = (w - SCR_W) // 2
            im = im.crop((x, 0, x + SCR_W, SCR_H))
            out = os.path.join(ROOT, "art", "bg", "_".join(parts[1:]) + ".png")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            im.save(out)
            report.append(f"фон      {f} -> {os.path.relpath(out, ROOT)}")
        else:
            # Персонаж: первые слова — кто и во что одет, последнее — эмоция.
            # В том же масштабе, что и экран, иначе пропорции поплывут.
            im = im.convert("RGBA")
            k = SCR_H / SRC_H
            w = max(2, round(im.size[0] * k) & ~1)
            h = round(im.size[1] * k)
            im = im.resize((w, h), Image.LANCZOS)
            who = "_".join(parts[:-1])
            out = os.path.join(ROOT, "art", "char", who, parts[-1] + ".png")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            im.save(out)
            sizes[who] = (w, h)
            report.append(f"спрайт   {f} -> {os.path.relpath(out, ROOT)} "
                          f"({w}x{h})")
    return sizes


# ---------------------------------------------------------------- перевод

def load_translation(game, lang):
    """Оригинальная реплика -> переведённая; и словарь строк.

    Сопоставляем по ТЕКСТУ оригинала, а не по номеру строки: файл перевода
    ссылается на строки той версии script.rpy, по которой он создавался, и
    после любой правки сценария номера разъезжаются. В блоке перевода
    оригинал записан комментарием прямо над переводом.
    """
    path = os.path.join(game, "tl", lang, "script.rpy")
    lines, strings = {}, {}
    if not lang or not os.path.exists(path):
        return lines, strings
    src = open(path, encoding="utf-8-sig").read().splitlines()
    orig = None
    old = None
    for raw in src:
        s = raw.strip()
        if s.startswith("# game/"):
            continue
        if s.startswith("# ") and split_say(s[2:]):
            orig = split_say(s[2:])
            continue
        if s.startswith("old "):
            old = literal(s[4:])
            continue
        if s.startswith("new ") and old is not None:
            strings[old] = literal(s[4:])
            old = None
            continue
        if orig and s and not s.startswith("translate"):
            r = split_say(s)
            if r:
                lines[orig] = r
            orig = None
    return lines, strings


def literal(s):
    """Строковый литерал Ren'Py -> строка. Файлы игры — чужие данные,
    поэтому только разбор литерала, никакого выполнения кода."""
    try:
        v = ast.literal_eval(s.strip())
    except (ValueError, SyntaxError):
        return s.strip().strip('"')
    return v if isinstance(v, str) else str(v)


def split_say(s):
    """'who "text"' или '"text"' -> (who или None, text)."""
    # Хвостовые модификаторы реплики (nointeract, with ...) отбрасываем:
    # Ren'Py дописывает их, например, к подписи меню в файле перевода.
    m = re.match(r'^(?:(\w+)\s+)?("(?:[^"\\]|\\.)*")(?:\s+(?:nointeract|with\s+\w+))*\s*$', s)
    if not m:
        return None
    return m.group(1), literal(m.group(2))


# ---------------------------------------------------------------- сценарий

def convert_script(game, lang, sizes, music, report):
    path = os.path.join(game, "script.rpy")
    src = open(path, encoding="utf-8-sig").read().splitlines()
    tl, strings = load_translation(game, lang)

    def tr(s):
        return strings.get(s, s)

    names, flags = {}, {}
    out = []
    shown = None                  # кто сейчас на экране
    pending_menu = None
    if_stack = []                 # (отступ, метка окончания блока)
    skipped = []
    n_if = 0

    def say(lineno, who, text):
        r = tl.get((who, text))
        if r:
            who, text = (r[0] or who), r[1]
        elif text in strings:
            # Подписи меню Ren'Py переводит не блоками, а строками old/new.
            text = strings[text]
        elif tl:
            skipped.append(f"{lineno}: нет перевода: {text[:40]}")
        out.append(f"name {names[who]}" if who in names else "name")
        out.append(clean_text(text))

    def char_cmd(who, attrs):
        key = who + "_" + attrs[0] if len(attrs) > 1 else who
        emo = attrs[-1]
        w, h = sizes.get(key, (160, 233))
        x = (SCR_W - w) // 2
        y = SCR_H - h
        return f"char 0 {key}/{emo} {x} {y}"

    i = 0
    while i < len(src):
        raw = src[i]
        lineno = i + 1
        i += 1
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip())

        # закрываем условные блоки, отступ которых кончился
        while if_stack and indent <= if_stack[-1][0]:
            out.append(f"@{if_stack.pop()[1]}")

        m = re.match(r'define\s+(\w+)\s*=\s*Character\(\s*_?\(?\s*"([^"]*)"', s)
        if m:
            names[m.group(1)] = clean_text(tr(m.group(2)))
            continue
        m = re.match(r"default\s+(\w+)\s*=\s*(True|False)", s)
        if m:
            flags[m.group(1)] = len(flags)
            continue
        m = re.match(r"label\s+(\w+)\s*:", s)
        if m:
            out.append("")
            out.append(f"@{m.group(1)}")
            if m.group(1) == "start":
                for f, n in flags.items():
                    out.append(f"set {n} 0")
            continue
        m = re.match(r"jump\s+(\w+)", s)
        if m:
            out.append(f"goto {m.group(1)}")
            continue
        if s == "return":
            out.append("bgm fadeout 90")
            out.append("fade out 1.5s")
            out.append("end")
            continue
        m = re.match(r"scene\s+(.+?)(?:\s+with\s+\w+)?$", s)
        if m:
            parts = m.group(1).split()
            bg = "black" if parts == ["black"] else "_".join(parts[1:])
            # Смена фона всегда через темноту: подмена на открытом экране
            # видна, а настоящего растворения у приставки нет.
            out.append("fade out 0.4s")
            # scene в Ren'Py убирает всё с экрана. Прячем спрайт ВСЕГДА, а
            # не только если «он показан»: сюда можно попасть переходом из
            # любой метки, и состояние экрана при чтении файла сверху вниз
            # не совпадает с тем, что будет у игрока.
            out.append("char 0 hide")
            shown = None
            out.append(f"bg {bg}")
            out.append("fade in 0.4s")
            continue
        if s.startswith("with "):
            continue                  # «with» на отдельной строке
        m = re.match(r"show\s+(\w+)((?:\s+\w+)+)", s)
        if m:
            attrs = m.group(2).split()
            out.append(char_cmd(m.group(1), attrs))
            shown = m.group(1)
            continue
        m = re.match(r"hide\s+(\w+)", s)
        if m:
            out.append("char 0 hide")
            shown = None
            continue
        m = re.match(r'play\s+music\s+"([^"]+)"', s)
        if m:
            if music:
                out.append(f"bgm play {music} loop")
            else:
                skipped.append(f"{lineno}: {s} (не задан --music)")
            continue
        if s.startswith("stop music"):
            out.append("bgm fadeout 60")
            continue
        m = re.match(r"\$\s*(\w+)\s*=\s*(True|False)$", s)
        if m and m.group(1) in flags:
            out.append(f"set {flags[m.group(1)]} {1 if m.group(2) == 'True' else 0}")
            continue
        m = re.match(r"if\s+(\w+)\s*:$", s)
        if m and m.group(1) in flags:
            n_if += 1
            end = f"_endif{n_if}"
            out.append(f"if {flags[m.group(1)]} 0 goto {end}")
            if_stack.append((indent, end))
            continue
        if s == "menu:":
            # Разбираем блок меню по отступам: подпись и варианты.
            caption, opts = None, []
            base = indent
            while i < len(src):
                r2 = src[i]
                s2 = r2.strip()
                ind2 = len(r2) - len(r2.lstrip())
                if s2 and ind2 <= base:
                    break
                i += 1
                if not s2:
                    continue
                if s2.endswith(":"):
                    r = split_say(s2[:-1])
                    # тело варианта: ищем jump
                    target = None
                    while i < len(src):
                        r3 = src[i]
                        s3 = r3.strip()
                        ind3 = len(r3) - len(r3.lstrip())
                        if s3 and ind3 <= ind2:
                            break
                        i += 1
                        mj = re.match(r"jump\s+(\w+)", s3)
                        if mj:
                            target = mj.group(1)
                    if r and target:
                        opts.append((clean_text(tr(r[1])), target))
                    else:
                        skipped.append(f"{i}: вариант меню без jump")
                else:
                    r = split_say(s2)
                    if r:
                        caption = (i, r)
            if caption:
                say(caption[0], caption[1][0], caption[1][1])
            out.append("choice")
            for t, lbl in opts:
                out.append(f'  "{t}" -> {lbl}')
            out.append("endchoice")
            continue
        r = split_say(s)
        if r:
            say(lineno, r[0], r[1])
            continue
        skipped.append(f"{lineno}: {s}")

    while if_stack:
        out.append(f"@{if_stack.pop()[1]}")

    report.extend("пропущено " + x for x in skipped)
    return out


def main():
    ap = argparse.ArgumentParser(description="перенос простой игры Ren'Py")
    ap.add_argument("game", help="каталог game/ игры Ren'Py")
    ap.add_argument("--lang", default="", help="язык из tl/, например russian")
    ap.add_argument("--music", default="", help="имя трека из music.txt")
    ap.add_argument("--body", default=os.path.join(ROOT, "script", "imported.vns"),
                    help="куда записать переведённый сценарий")
    args = ap.parse_args()

    report = []
    sizes = convert_images(args.game, report)
    body = convert_script(args.game, args.lang, sizes, args.music, report)
    os.makedirs(os.path.dirname(args.body), exist_ok=True)
    open(args.body, "w", encoding="utf-8").write("\n".join(body) + "\n")
    for r in report:
        print(r)
    print(f"сценарий: {os.path.relpath(args.body, ROOT)}, "
          f"строк {len(body)}")


if __name__ == "__main__":
    main()
