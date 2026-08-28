# -*- coding: utf-8 -*-
"""Arabic for the LAN screens (lan_ui.cpp / net_lan.cpp).

These are NOT retail strings -- they are rows this port added, and unlike the retail table
most of them are rebuilt every frame from live session state ("YOUR NAME: <name>",
"SKIN 2/4", "3/4 - 21MS"). So they cannot be whole pack strings the way a menu label is:
only the FIXED part is translated here, and lan_ui composes it with the live value, in
reading order, leaving the runtime shaper to place the digits.

Deliberately NOT translated:
  * the on-screen keyboard grid -- it is how a player types their name, and the names,
    passwords and IP addresses they type are Latin
  * host names off the network, which are whatever the other machine calls itself
  * "MS" (milliseconds) and the skin/slot numerals

The ids are shared with lan_ui.cpp's LanText enum; the values are pack indices from 0x200.
"""

LAN = {
    # --- browser (screen 5) ---------------------------------------------------
    0x200: 'شبكة محلية',                 # LAN GAME (title)
    0x201: 'اسمك:',                      # YOUR NAME:
    0x202: 'كلمة السر:',                 # PASSWORD:
    0x203: 'لا توجد',                        # NONE
    0x204: 'استضف لعبة',                 # HOST A GAME
    0x205: 'انضم بالعنوان',              # JOIN BY ADDRESS
    0x206: 'الألعاب على هذه الشبكة',     # GAMES ON THIS NETWORK
    0x207: 'إصدار مختلف',                # DIFFERENT VERSION
    0x208: 'في مباراة',                  # IN A MATCH
    0x209: 'ممتلئة',                     # FULL
    0x20A: 'مقفلة',                      # LOCKED
    0x20B: 'مفتوحة',                     # OPEN
    0x20C: 'سريعة',                      # QUICK   (browser mode column)
    0x20D: 'بطولة',                      # TOURNEY (browser mode column)
    0x20E: 'لا توجد ألعاب بعد - هل الجهاز الآخر على هذه الشبكة؟',
    # --- lobby (screen 15) ----------------------------------------------------
    0x210: 'ردهة الشبكة',                # LAN LOBBY
    0x211: 'مستضيف',                     # HOSTING
    0x212: 'منضم',                       # JOINED
    0x213: 'أنت',                        # YOU  (slot heading, with the seat number)
    0x214: 'لاعب',                       # PLAYER (slot heading, with the seat number)
    0x215: 'حاسوب',                      # COMPUTER (a filled CPU seat)
    0x216: 'مظهر',                         # SKIN
    0x217: 'شاغر',                       # OPEN (an empty seat)
    0x218: 'إزالة؟',                     # REMOVE?
    0x219: 'فريق',                       # TEAM
    0x21A: 'الساحة:',                    # ARENA:
    0x21B: 'إعدادات القتال',             # FIGHT SETTINGS
    0x21C: 'الوضع:',                     # MODE:
    0x21D: 'ابدأ المباراة',              # START MATCH
    0x21E: 'مباراة سريعة',               # QUICK MATCH  (lobby mode row)
    0x21F: 'بطولة',                      # TOURNAMENT
    0x220: 'سباق اللحم',                 # MEAT RUSH
    0x221: 'الاتجاهات للتحرك',           # D-PAD MOVE
    0x222: 'تغيير',                      # CHANGE
    0x223: 'مغادرة',                     # LEAVE
    0x224: 'اضغط زر البدء للبدء',        # PRESS START TO BEGIN
    0x225: 'جاهز - في انتظار المستضيف',  # READY - WAITING FOR THE HOST
    0x226: 'اضغط زر البدء عندما تكون جاهزا',   # PRESS START WHEN YOU ARE READY
    0x227: 'جاهز',                       # READY (the suffix on a ready seat's team cell)
    # --- text-entry modal -----------------------------------------------------
    # The on-screen keyboard's three ACTION keys. The LETTER keys stay Latin -- they are
    # what a name, a password and an IP address are made of -- but these three are labels,
    # not characters, and the player never types them.
    0x228: 'حذف',                        # DELETE
    0x229: 'مسافة',                      # SPACE
    0x22A: 'تم',                         # DONE
    0x230: 'أدخل اسمك',                  # ENTER YOUR NAME
    0x231: 'اضبط كلمة سر - اتركها فارغة للعبة مفتوحة',
    0x232: 'أدخل عنوان المستضيف',        # ENTER THE HOST'S ADDRESS
    0x233: 'هذه اللعبة مقفلة',           # THIS GAME IS LOCKED
    # --- refusals (net_lan.cpp) -----------------------------------------------
    0x240: 'تلك اللعبة ممتلئة',
    0x241: 'ذلك اللاعب لديه إصدار مختلف من اللعبة',
    0x242: 'ذلك اللاعب لديه ملفات لعبة مختلفة',
    0x243: 'إصدار مختلف - حدث الجهازين',
    0x244: 'تلك اللعبة بدأت بالفعل',
    0x245: 'كلمة سر خاطئة',
    # --- shared words and the status line the browser shows while idle ---------
    0x250: 'رجوع',                       # BACK   (footer, reuses the retail wording)
    0x251: 'اختيار',                     # SELECT
    0x252: 'إلغاء',                      # CANCEL
    0x253: 'جار البحث عن الألعاب...',    # SEARCHING FOR GAMES...
    0x254: 'يستضيف على المنفذ',          # HOSTING ON PORT <n>
    0x255: 'جار الانضمام...',            # JOINING...
    0x256: 'جار بدء المباراة...',        # STARTING MATCH...
    # 0x257 CONNECTING TO <host> is deliberately NOT translated: it costs the one
    # remaining glyph slot, and it is a transient line followed by a Latin host name.
    # --- why the host cannot start yet (net_lan.cpp LanStartRefusal) ----------
    # All five share the lobby's one prompt slot, so all five must be translated or the
    # screen flips to English at the moment it most needs to be read.
    0x260: 'لست المستضيف',
    0x261: 'في انتظار جاهزية الجميع',
    0x262: 'يلزم لاعب واحد على الأقل',
    0x263: 'يلزم لاعبان على الأقل',
    0x264: 'يلزم فريقان مختلفان على الأقل',
    # --- the browser's status line (net_lan.cpp Status) ------------------------
    # Only the lines a player reads in normal play. The Winsock/UDP diagnostics stay
    # English on purpose: they name error codes and exist to be searched for.
    0x270: 'انضم',                       # <name> JOINED   -- composed in reading order
    0x271: 'غادر',                       # <name> LEFT
    0x272: 'انضممت - المقعد',            # JOINED - SLOT <n>
    0x273: 'غادر المستضيف اللعبة',
    0x274: 'أزالك المستضيف',
    0x275: 'تمت إزالة لاعب',
    0x276: 'تمت إزالته',                 # <name> WAS REMOVED
    0x277: 'عدنا إلى الردهة',
    0x278: 'تم فصل وحدة تحكم',
    0x279: 'لا رد من تلك اللعبة',
    0x27A: 'انقطع اتصال لاعب',
    0x27B: 'إعدادات غير متطابقة - ألغي البدء',
    0x27C: 'أدخل عنوانا أو اسم جهاز',
    0x27D: 'تعذر الوصول إلى',            # CANNOT FIND <host>
    0x27E: 'جار الاتصال',                # CONNECTING TO <host>
}
