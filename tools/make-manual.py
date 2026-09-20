# Builds the user manual that ships with the installer:
#   python tools/make-manual.py        ->  docs/manual/USDprep_User_Manual.pdf
# One manual, for the compositor who uses the program; installation is a
# short chapter at the end because the installer leaves little to do.
# Screenshots live in docs/manual/img (tools/capture-window.ps1 takes them).
import os, re

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (BaseDocTemplate, Frame, Image, KeepTogether, PageBreak, PageTemplate, Paragraph,
                                Spacer, Table, TableStyle)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "docs", "manual", "img")
OUT = os.path.join(ROOT, "docs", "manual", "USDprep_User_Manual.pdf")
VERSION = re.search(r'USDPREP_VERSION_STRING\s+"([^"]+)"',
                    open(os.path.join(ROOT, "src", "core", "include", "usdprep", "Version.h")).read()).group(1)

ACCENT = colors.HexColor("#2E8B47")   # the green of the Export button
INK = colors.HexColor("#1E2329")
MUTED = colors.HexColor("#5C6670")
RULE = colors.HexColor("#D5DAE0")
SHADE = colors.HexColor("#F3F5F7")

base = getSampleStyleSheet()
BODY = ParagraphStyle("body", parent=base["Normal"], fontName="Helvetica", fontSize=10, leading=14.5,
                      textColor=INK, spaceAfter=6, alignment=TA_LEFT)
SMALL = ParagraphStyle("small", parent=BODY, fontSize=8.5, leading=12, textColor=MUTED)
H1 = ParagraphStyle("h1", parent=BODY, fontName="Helvetica-Bold", fontSize=19, leading=24, spaceBefore=4,
                    spaceAfter=10, textColor=INK)
H2 = ParagraphStyle("h2", parent=BODY, fontName="Helvetica-Bold", fontSize=12.5, leading=17, spaceBefore=12,
                    spaceAfter=5, textColor=ACCENT)
CELL = ParagraphStyle("cell", parent=BODY, fontSize=9, leading=12.5, spaceAfter=0)
CELLB = ParagraphStyle("cellb", parent=CELL, fontName="Helvetica-Bold")
STEP = ParagraphStyle("step", parent=BODY, leftIndent=16, bulletIndent=0, spaceAfter=4)
CODE = ParagraphStyle("code", parent=BODY, fontName="Courier", fontSize=8.8, leading=12.5, backColor=SHADE,
                      borderPadding=(5, 6, 5, 6), spaceBefore=4, spaceAfter=10, leftIndent=2)
CAPTION = ParagraphStyle("caption", parent=SMALL, spaceBefore=3, spaceAfter=10)

WIDTH = A4[0] - 40 * mm


def p(text, style=BODY):
    return Paragraph(text, style)


def steps(items):
    return [Paragraph(text, STEP, bulletText="%d." % (i + 1)) for i, text in enumerate(items)]


def bullets(items):
    return [Paragraph(text, STEP, bulletText="•") for text in items]


def picture(name, width, caption=None):
    path = os.path.join(IMG, name)
    img = Image(path)
    ratio = img.imageHeight / float(img.imageWidth)
    img.drawWidth, img.drawHeight = width, width * ratio
    img.hAlign = "LEFT"
    out = [img]
    if caption:
        out.append(p(caption, CAPTION))
    return out


def table(rows, widths, header=True):
    data = [[Paragraph(str(c), CELLB if (header and r == 0) else CELL) for c in row] for r, row in enumerate(rows)]
    t = Table(data, colWidths=widths, repeatRows=1 if header else 0)
    style = [("VALIGN", (0, 0), (-1, -1), "TOP"), ("LINEBELOW", (0, 0), (-1, -1), 0.4, RULE),
             ("TOPPADDING", (0, 0), (-1, -1), 4), ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
             ("LEFTPADDING", (0, 0), (-1, -1), 5), ("RIGHTPADDING", (0, 0), (-1, -1), 5)]
    if header:
        style += [("BACKGROUND", (0, 0), (-1, 0), SHADE), ("LINEBELOW", (0, 0), (-1, 0), 0.8, ACCENT)]
    t.setStyle(TableStyle(style))
    t.hAlign = "LEFT"
    return t


def side_by_side(image_name, image_width, flow):
    img = picture(image_name, image_width)[0]
    t = Table([[img, flow]], colWidths=[image_width + 6 * mm, WIDTH - image_width - 6 * mm])
    t.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "TOP"), ("LEFTPADDING", (0, 0), (-1, -1), 0),
                           ("RIGHTPADDING", (0, 0), (-1, -1), 0), ("TOPPADDING", (0, 0), (-1, -1), 0)]))
    return t


def on_page(canvas, doc):
    canvas.saveState()
    canvas.setStrokeColor(RULE)
    canvas.setLineWidth(0.5)
    canvas.line(20 * mm, 15 * mm, A4[0] - 20 * mm, 15 * mm)
    canvas.setFont("Helvetica", 8)
    canvas.setFillColor(MUTED)
    canvas.drawString(20 * mm, 10.5 * mm, "USDprep %s - User Manual" % VERSION)
    canvas.drawRightString(A4[0] - 20 * mm, 10.5 * mm, str(doc.page))
    canvas.restoreState()


def on_cover(canvas, doc):
    canvas.saveState()
    canvas.setFillColor(INK)
    canvas.rect(0, A4[1] - 92 * mm, A4[0], 92 * mm, stroke=0, fill=1)
    canvas.setFillColor(ACCENT)
    canvas.rect(0, A4[1] - 94 * mm, A4[0], 2 * mm, stroke=0, fill=1)
    canvas.setFillColor(colors.white)
    canvas.setFont("Helvetica-Bold", 40)
    canvas.drawString(20 * mm, A4[1] - 52 * mm, "USDprep")
    canvas.setFont("Helvetica", 15)
    canvas.drawString(20 * mm, A4[1] - 64 * mm, "USD scenes made ready for Nuke")
    canvas.setFont("Helvetica", 10)
    canvas.setFillColor(colors.HexColor("#B9C2CB"))
    canvas.drawString(20 * mm, A4[1] - 78 * mm, "User Manual  -  version %s" % VERSION)
    canvas.restoreState()


story = []

# ------------------------------------------------------------------ cover
story += [Spacer(1, 84 * mm)]
story += picture("app.png", WIDTH)
story += [Spacer(1, 4 * mm),
          p("USDprep opens the heavy USD scene the CG department delivered, lets you pick the one object you need "
            "by clicking it, and writes a small file that Nuke reads - geometry, textures and animation. "
            "You do not need to know USD to use it."),
          p("Scene in the screenshots: ALab, by Animal Logic (open-source production scene).", SMALL),
          Spacer(1, 6 * mm),
          p("In this manual", H2),
          table([["1", "Quick start - five steps from a scene to geometry in your comp"],
                 ["2", "Finding your way around - the 3D view, the object list, the keys"],
                 ["3", "Exporting - what, how, and which format for which Nuke"],
                 ["4", "Opening the result in Nuke"],
                 ["5", "The Advanced switches"],
                 ["6", "What USDprep changes, and why"],
                 ["7", "If something looks wrong"],
                 ["8", "For pipeline TDs: the command line"],
                 ["9", "Installation"]], [10 * mm, WIDTH - 10 * mm], header=False),
          PageBreak()]

# ------------------------------------------------------------ quick start
story += [p("1  Quick start", H1),
          p("Five steps from a production scene to geometry in your comp.")]
story += steps([
    "<b>Open the scene</b> with <i>File &gt; Open</i> (<font face='Courier'>.usd, .usda, .usdc</font> or "
    "<font face='Courier'>.usdz</font>). The scene is never modified - USDprep only reads it.",
    "<b>Pick the object.</b> Click it in the 3D view, or click its name in the list on the right. "
    "It turns yellow in both places. One click takes the whole object with everything inside it.",
    "<b>Check the name and place</b> in <i>Save as</i>. USDprep suggests the object's name, in the folder you "
    "used last.",
    "<b>Press the green Export button.</b> A line under the button says what was written and how big it is.",
    "<b>In Nuke</b>, read the <font face='Courier'>.usdc</font> file with a <i>GeoImport</i> node. "
    "Keep the file and the <font face='Courier'>_textures</font> folder next to it together.",
])
story += [Spacer(1, 3 * mm)]
story += picture("app_start.png", WIDTH, "USDprep after opening a scene: the 3D view on the left, the "
                                         "<i>Prep for Nuke</i> panel on the right. Nothing is selected yet.")
story += [p("That is the whole workflow. The rest of this manual explains the choices you can make on the way, "
            "what USDprep changes in the file and why, and what to do with an older Nuke."),
          PageBreak()]

# ------------------------------------------------------- finding your way
story += [p("2  Finding your way around", H1)]
story += [side_by_side("panel_top.png", 62 * mm, [
    p("The top of the panel shows the scene's name and how many objects it has. "
      "<b>Frame selection (F)</b> moves the camera to the selected object. "
      "<b>Whole scene (A)</b> is the way back from anywhere: it shows where the objects are. "
      "<b>Clear selection</b> deselects everything."),
    p("Hover over <i>How to move (?)</i> for the mouse controls; they are also listed below."),
])]
story += [p("Moving in the 3D view", H2)]
story += [table([
    ["Do this", "To"],
    ["Mouse wheel", "Move closer or further"],
    ["Alt + left drag", "Turn around the object"],
    ["Alt + middle drag", "Slide sideways, up and down"],
    ["Alt + right drag", "Move closer or further, smoothly"],
    ["Hold the right button + W A S D  (Q / E = down / up)", "Walk through the scene; Shift walks faster"],
    ["F", "Frame the selected object"],
    ["A", "Whole scene - where the objects are"],
], [78 * mm, WIDTH - 78 * mm])]
story += [Spacer(1, 3 * mm),
          p("<b>Seeing inside rooms.</b> When you frame an object that stands in a room, the wall between the camera "
            "and the object is cut away so that you can see it. The walls come back when you press A or walk with "
            "the right mouse button. Nothing is removed from the scene - it is only the view.")]
story += [p("The object list", H2)]
story += [side_by_side("panel_tree.png", 62 * mm, [
    p("The list shows the scene as folders within folders. Click the small triangle to open a folder; "
      "click a <b>name</b> to select that object with everything inside it. Click the name again to deselect."),
    p("The selected object is <font color='#B8860B'><b>yellow</b></font>; what belongs to it is a paler yellow. "
      "The number after a name says how many parts the object has."),
    p("List and 3D view follow each other: click an object in 3D and the list opens exactly the folders needed "
      "and scrolls it into the middle."),
    p("<b>Up arrow</b> selects the parent - the bigger object this one belongs to. <b>Down arrow</b> goes back "
      "down. Use them when a click picked a smaller or bigger piece than you wanted."),
    p("<b>Ctrl + click</b> adds an object to the selection, or takes one out, without touching the rest."),
    p("<b>Search objects...</b> filters the list by name while you type."),
])]
story += [PageBreak()]

# ----------------------------------------------------------------- export
story += [p("3  Exporting", H1)]
story += [side_by_side("panel_export.png", 70 * mm, [
    p("<b>What gets exported.</b> Normally: whatever is selected. If you need several objects from different "
      "places, select the first, press <b>Add to export</b>, select the next, add it, and so on. The list keeps "
      "them; clicking an entry shows you that object again. <b>Clear list</b> empties it. "
      "All of them go into one file."),
    p("<b>Preset.</b> <i>Nuke-ready</i> is the one to use: it makes the file small and makes sure Nuke can read "
      "everything in it. <i>Original (nothing changed)</i> only cuts the object out and leaves it exactly as the "
      "CG department made it - Nuke may not show parts of such a file correctly. "
      "<i>Recipe file...</i> loads settings your pipeline TD prepared."),
    p("<b>Save as.</b> Type a name or use <i>Browse...</i>. The file extension follows the format below it."),
    p("<b>Format.</b> See the table on this page."),
    p("<b>Advanced</b> holds every individual switch; chapter 5 explains them. You rarely need it."),
    p("After the export the line under the button reports the result. <b>Details</b> opens the full report: "
      "everything USDprep did to the file, in plain words. If something in Nuke looks different from what you "
      "expected, the reason is in that report."),
])]
story += [p("Which format for which Nuke", H2)]
story += [table([
    ["Format", "Use it for", "What is inside", "What you get"],
    ["<b>USD (.usdc)</b>", "Nuke with the current, USD-based 3D system (GeoImport, ScanlineRender2). "
                           "Checked with Nuke 16.1 and 17.0.",
     "Geometry, materials with their textures, animation, cameras.",
     "<font face='Courier'>name.usdc</font><br/><font face='Courier'>name_textures/</font>"],
    ["<b>Alembic (.abc)</b>", "An older Nuke that only has the classic 3D system (ReadGeo). Animated objects.",
     "Geometry with UVs and its animation. No materials - Nuke reads none from this format.",
     "<font face='Courier'>name.abc</font><br/><font face='Courier'>name_textures/</font><br/>"
     "<font face='Courier'>name_abc.nk</font>"],
    ["<b>OBJ (.obj)</b>", "An older Nuke, objects that do not move.",
     "Geometry with UVs at one frame. No animation, no materials.",
     "<font face='Courier'>name.obj</font><br/><font face='Courier'>name_textures/</font><br/>"
     "<font face='Courier'>name_obj.nk</font>"],
], [27 * mm, 50 * mm, 50 * mm, WIDTH - 127 * mm])]
story += [Spacer(1, 2 * mm),
          p("USDprep offers only formats that Nuke was measured to read. A <font face='Courier'>.usdz</font> package "
            "is not among them: Nuke loads its geometry but none of the textures inside it.", SMALL),
          PageBreak()]

# ---------------------------------------------------------------- in Nuke
story += [p("4  Opening the result in Nuke", H1)]
story += [p("USD - the current 3D system", H2)]
story += steps([
    "Create a <b>GeoImport</b> node and point it at the <font face='Courier'>.usdc</font> file.",
    "Connect it to a <b>ScanlineRender2</b> together with your camera. Materials and textures are already "
    "in the file.",
    "Without any light in the Nuke scene the object shows its plain texture colours, which is usually what a "
    "comp needs. As soon as you add a light, it is lit by that light.",
])
story += [p("Keep the <font face='Courier'>.usdc</font> and its <font face='Courier'>_textures</font> folder "
            "together when you move or copy them - the file points at the textures next to it.")]
story += [p("Alembic and OBJ - the classic 3D system", H2)]
story += [p("Nuke reads no materials from these two formats, so USDprep writes a small Nuke script next to the "
            "file that sets everything up for you:")]
story += steps([
    "In Nuke choose <b>File &gt; Insert Comp Nodes...</b> and pick "
    "<font face='Courier'>name_abc.nk</font> (or <font face='Courier'>name_obj.nk</font>).",
    "You get one <b>ReadGeo</b> node per material, each with its texture already connected, and a <b>Scene</b> "
    "node that joins them. Connect the Scene to your ScanlineRender.",
    "For an <font face='Courier'>.abc</font>, set the Nuke project to the frame rate of the scene; the export "
    "report tells you which one it is. The animation sits on the scene's own frame numbers (for example "
    "1004-1057).",
])
story += [p("When an object has several materials, the geometry is also written once per material into a "
            "<font face='Courier'>name_abc_parts</font> (or <font face='Courier'>name_obj_parts</font>) folder - "
            "a classic ReadGeo takes only one texture for everything it reads, and the script uses those files. "
            "The paths inside the script are absolute: if you move the export to another place, export again or "
            "fix the paths in the Read and ReadGeo nodes."),
          p("If you load an <font face='Courier'>.abc</font> by hand instead, Nuke asks which items to load - "
            "choose all of them.")]
story += [PageBreak()]

# --------------------------------------------------------------- advanced
story += [p("5  The Advanced switches", H1),
          p("Every change USDprep makes to the file has its own switch here, so you are always one click away "
            "from the original. The preset sets them; you can change any of them for one export.")]
story += [table([
    ["Switch", "What it does", "Nuke-ready"],
    ["De-instance", "Production scenes reuse one object many times (instancing). This turns the copies into "
                    "ordinary objects. Nuke reads instancing, but its render stops with \"Too many open files\" "
                    "on a scene with over a thousand instances. The file does not get larger.", "on"],
    ["Set main object", "Marks the main object of the file, which applications use to know what to load.", "on"],
    ["Copy textures next to the file", "Copies every texture the object uses into the "
                                       "<font face='Courier'>_textures</font> folder and points the file at the "
                                       "copies. Off: the file keeps pointing into the production folders.", "on"],
    ["Materials", "Production objects often carry two materials: a heavy one for final renders and a light one "
                  "for previews. <i>Light</i> keeps the small one, which is what a comp needs; "
                  "<i>Full quality</i> keeps the heavy one; <i>Keep both</i> leaves them as they are.", "Light"],
    ["Animation", "<i>Shot range only</i> drops animation from before and after the shot (simulations often "
                  "start a long time before it). <i>One frame</i> freezes the object at the frame you type. "
                  "<i>Everything</i> keeps every sample.", "Shot range"],
    ["Textures", "Scales textures larger than the chosen size down - in the exported copy only, the originals "
                 "are never touched.", "At most 4K"],
    ["Geometry", "Reduces the number of polygons of dense objects (half, a quarter, a tenth), keeping the UVs. "
                 "It changes the shape slightly and is never on by itself.", "As it is"],
    ["Include lights", "Off by default: with a light in the file Nuke stops showing objects in their plain "
                       "colours and the picture goes dark. On: the light types Nuke can use come along; the "
                       "others become axes of the same name in the same place, so you can rebuild them.", "off"],
    ["Remove guide and proxy geometry", "Stand-in geometry the asset carries for other programs. Nuke draws all "
                                        "of it, so the object would show twice. Off: it stays in the file, "
                                        "hidden.", "on"],
    ["Remove renderer-only shader networks", "Material parts only Arnold, RenderMan and similar renderers "
                                             "understand, with the textures only they use.", "on"],
    ["Remove unused materials", "Materials nothing in the export uses.", "on"],
    ["Remove preview cards", "Six small pictures some viewers show instead of the object. Nuke never draws "
                             "them.", "on"],
], [42 * mm, WIDTH - 64 * mm, 22 * mm])]
story += [PageBreak()]

# ------------------------------------------------------------ conversions
story += [p("6  What USDprep changes, and why", H1),
          p("Nuke does not read everything a USD file can contain. What Nuke 16.1 and 17.0 actually read was "
            "measured with test scenes, and the <i>Nuke-ready</i> preset converts what they cannot read into the "
            "closest thing they can. Nothing is thrown away silently: every conversion is listed, by name, in "
            "the report behind the <b>Details</b> button.")]
story += [table([
    ["In the scene", "What Nuke would do", "What USDprep does"],
    ["Textures split into UDIM tiles", "Black material", "Joins the tiles into one texture and adjusts the "
                                                           "material to use it"],
    ["Lights Nuke cannot use (distant, rectangular, cylinder...)", "Black or wrong lighting",
     "Replaces each by an axis of the same name in the same place (only when <i>Include lights</i> is on)"],
    ["Guide and proxy geometry", "Draws it on top of the real object", "Removes it, or hides it if you keep it"],
    ["MaterialX next to a standard material", "Black material", "Removes the MaterialX part"],
    ["A material only a production renderer understands", "The object disappears",
     "Detaches the object from it; it shows in its plain display colour"],
    ["Characters moved by a skeleton", "Stands frozen in its rest pose", "Bakes the motion into the geometry"],
    ["A scene built with Z pointing up", "Lies on its back", "Stands it up"],
    ["One object with several materials on different faces", "Grey object",
     "Splits it into one object per material, inside a group with the original name"],
    ["Basic shapes (sphere, cube, cylinder, cone, capsule)", "Draws nothing", "Turns them into ordinary geometry"],
    ["Curves (hair, whiskers, wires)", "Draws nothing", "Cannot be converted - the report tells you they are there"],
], [52 * mm, 42 * mm, WIDTH - 94 * mm])]

# -------------------------------------------------------- troubleshooting
story += [p("7  If something looks wrong", H1)]
story += [table([
    ["What you see", "Likely reason and what to do"],
    ["The object is black in Nuke", "The textures are not where the file expects them: keep the "
                                    "<font face='Courier'>_textures</font> folder next to the file. "
                                    "If you exported with <i>Original</i>, export again with <i>Nuke-ready</i>."],
    ["The object is dark", "There is a light in the file or in your Nuke scene. Export without "
                           "<i>Include lights</i>, or light the object in Nuke."],
    ["A part is missing", "Open <b>Details</b> after the export: the report lists what was removed or could not "
                          "be converted (for example curves). Also check that the part was inside your "
                          "selection - press the up arrow to select the bigger object."],
    ["The report says textures were left out", "Those texture files are not on this computer; the scene "
                                               "points at a place you cannot reach. Ask for the textures."],
    ["Alembic in Nuke does not move, or moves at the wrong speed", "Set the Nuke project's frame rate to the "
                                                                   "one named in the report, and look at the "
                                                                   "scene's frame numbers."],
    ["Only one piece of an .abc shows", "You read it by hand and picked one item. Use the "
                                        "<font face='Courier'>.nk</font> script, or select all items in "
                                        "the dialog."],
    ["The 3D view stays empty or grey", "The graphics driver is too old (OpenGL 4.5 is needed). Update the "
                                        "driver from NVIDIA, AMD or Intel. A computer that runs Nuke 16 or 17 "
                                        "is new enough."],
    ["I am lost in the 3D view", "Press <b>A</b>."],
], [48 * mm, WIDTH - 48 * mm])]
story += [PageBreak()]

# ------------------------------------------------------------ command line
story += [p("8  For pipeline TDs: the command line", H1),
          p("Everything the panel does is also available without a window, for scripts and render farms. "
            "<font face='Courier'>usdcut</font> is installed next to the program "
            "(<font face='Courier'>bin\\usdcut.exe</font>); tick <i>Add the usdcut command line tool to my PATH</i> "
            "in the installer to call it from anywhere.")]
story += [p("usdcut extract scene.usd /World/Set/Car -o car.usdc --preset nuke<br/>"
            "usdcut extract scene.usd /World/Char/Hero -o hero.abc --preset nuke --frames 1001-1080<br/>"
            "usdcut extract scene.usd /World/Set/Car -o car.obj --preset nuke --frame 1010<br/>"
            "usdcut select  scene.usd --name \"*door*\" --topmost<br/>"
            "usdcut inspect scene.usd<br/>"
            "usdcut presets nuke &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;(prints the preset as JSON - the start of "
            "your own recipe)<br/>"
            "usdcut help", CODE)]
story += [p("A recipe is a small JSON file with the same switches as the Advanced section. Start from the printed "
            "preset, change the lines you need, and give it to artists to load with <i>Recipe file...</i> or pass "
            "it as <font face='Courier'>--recipe studio.json</font>. Add <font face='Courier'>--report out.json</font> "
            "to get the export report as a file.")]

# ------------------------------------------------------------ installation
story += [p("9  Installation", H1),
          p("Run <font face='Courier'>USDprep-%s-setup.exe</font> and follow its pages. That is all." % VERSION)]
story += bullets([
    "It installs for <b>you only</b>: no administrator rights and no internet connection are needed.",
    "Everything the program needs is inside its own folder "
    "(by default <font face='Courier'>%LOCALAPPDATA%\\Programs\\USDprep</font>). It does not touch Nuke, Python "
    "or any other USD installation on the computer.",
    "It adds a <b>USDprep</b> entry to the Start menu. A desktop shortcut and the <font face='Courier'>usdcut</font> "
    "entry in your PATH are optional tick boxes.",
    "This manual is installed with the program; the Start menu has a shortcut to it.",
    "<b>To remove it</b>, use <i>Settings &gt; Apps</i> or <i>Uninstall USDprep</i> in the Start menu. "
    "Nothing is left behind.",
])
story += [p("What the computer needs", H2)]
story += bullets([
    "Windows 10 or 11, 64-bit.",
    "A graphics card with a current driver from its vendor (OpenGL 4.5). Any computer that runs Nuke 16 or 17 "
    "qualifies. Without it the 3D view stays empty; <font face='Courier'>usdcut</font> works regardless.",
    "About 100 MB of disk space.",
])
story += [p("Licence", H2),
          p("USDprep is free software under the Apache License 2.0 - free to use, change and redistribute, also "
            "commercially. It is built on usdtweak by Cyril Pichard (Apache 2.0), Pixar's OpenUSD, Alembic, Imath "
            "and meshoptimizer. Their licences are in the <font face='Courier'>licenses</font> folder of the "
            "installation, listed in <font face='Courier'>THIRD_PARTY_NOTICES.txt</font>.")]

doc = BaseDocTemplate(OUT, pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm, topMargin=18 * mm,
                      bottomMargin=20 * mm, title="USDprep User Manual", author="USDprep",
                      subject="USD scenes made ready for Nuke")
frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="body", leftPadding=0, rightPadding=0,
              topPadding=0, bottomPadding=0)
doc.addPageTemplates([PageTemplate(id="cover", frames=[frame], onPage=on_cover, autoNextPageTemplate="page"),
                      PageTemplate(id="page", frames=[frame], onPage=on_page)])
doc.build(story)
print("manual :", OUT, "(%.0f KB)" % (os.path.getsize(OUT) / 1024.0))
