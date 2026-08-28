# -*- coding: utf-8 -*-
"""Arabic translation of the game's string table (retail index -> logical Arabic).

Indices are the retail ones (`FUN_00019910`'s table, 227 entries).  Anything absent here
keeps its English text, so the Latin glyphs must stay installed -- see arabic_font.py, which
frees only the glyph slots whose characters no *retained* English string can use.

TWO ENGINE ESCAPES ARE COPIED THROUGH VERBATIM and must keep their two-byte shape:
  \x07 + '0'/'1'  inline colour (retail marks every settings VALUE orange with \x071)
  \x08 + letter   button glyph sprite (X = A button, O = B, S/T/D = others). UPPERCASE is
                  the full-size icon and is what every English prompt uses; lowercase is a
                  half-size variant. A LEADING one is kept leading by the shaper, so the
                  full-size overhang falls into the margin exactly as it does in English.

Strings are stored in READING order and shaped at RUNTIME (arabic.cpp), which is what lets a
translated string carry a printf conversion: the game's vsprintf substitutes the value first
and the shaper runs afterwards, on the finished line.

DELIBERATELY LEFT IN ENGLISH:
  0x00-0x05  language names -- endonyms, they name languages in their own script
  0x2B-0x2D  the trademark / copyright blocks
  0xA5-0xA8  HUMAN_1P..HUMAN_4P, internal identifiers the UI never shows
"""

STRINGS = {
    # --- characters -----------------------------------------------------------
    0x06: 'توم وجيري',
    0x07: 'توم',
    0x08: 'جيري',
    0x09: 'سبايك',
    0x0A: 'بوتش',
    0x0B: 'نيبلز',
    0x0C: 'البطة',
    0x0D: 'تايك',
    0x0E: 'النسر',
    0x0F: 'جيري الوحش',
    0x10: 'القط الآلي',
    0x11: 'الأسد',
    # --- players / hud --------------------------------------------------------
    0x12: 'اللاعب 1',
    0x13: 'اللاعب 2',
    0x14: 'شاشة عريضة:',
    0x15: '\x071تشغيل',
    0x16: '\x071إيقاف',
    0x17: 'جار التحميل...',
    0x18: 'إيقاف مؤقت',
    0x19: 'هياج',
    0x1A: 'استعداد',
    0x1B: 'الهياج نشط',
    0x1C: 'تعادل الجولة',
    # --- main menu ------------------------------------------------------------
    0x1D: 'التحدي',
    0x1E: 'الخيارات',
    0x1F: 'حفظ اللعبة',
    0x20: 'مواجهة',
    0x21: 'اللغات',
    0x22: 'مواجهة بالتبديل',
    0x23: 'فريق بالتبديل',
    0x25: '\x08X إعادة المحاولة',
    0x26: '\x08O المتابعة دون حفظ',
    0x27: 'نعم',
    0x28: 'لا',
    0x29: 'تم التحميل بنجاح',
    0x2A: 'تم الحفظ بنجاح',
    0x2E: 'اضغط زر البدء',
    0x2F: 'اضغط \x08x للمتابعة',
    # --- match ----------------------------------------------------------------
    0x30: 'حدد القوة',
    0x31: 'استعد!',
    0x32: 'الفريق 1',
    0x33: 'الفريق 2',
    0x34: 'فاز!',
    0x35: 'خسر!',
    0x36: 'تعادل!',
    0x37: 'قاتل!',
    0x38: '\x08O رجوع',
    0x39: '\x08X اختيار',
    0x3A: '\x08O إلغاء',
    # --- fight settings -------------------------------------------------------
    0x3B: 'الوقت:',
    0x3F: '\x071غير محدود',
    0x40: 'الجولات:',
    0x44: 'الصعوبة:',
    0x45: '\x071سهل',
    0x46: '\x071متوسط',
    0x47: '\x071صعب',
    0x48: 'الصوت',
    0x49: 'الصورة',
    0x4A: 'إعدادات القتال',
    0x4B: 'الاهتزاز:',
    0x4C: 'الموسيقى:',
    0x4D: 'المؤثرات:',
    0x59: 'صناع اللعبة',
    0x5A: 'تأكيد',
    0x5B: 'قائمة الأكواد',
    0x5C: 'أدخل الكود:',
    0x5D: '\x08X\x08O\x08T\x08S أدخل الكود',
    0x5E: 'كود غير صالح',
    0x5F: 'تم قبول الكود',
    # --- character select -----------------------------------------------------
    0x60: 'اختيار اللاعب 1',
    0x61: 'اختيار اللاعب 2',
    0x62: 'اختيار الحاسوب',
    0x63: 'اللاعب 1',
    0x64: 'اللاعب 2',
    0x65: 'الحاسوب',
    0x66: 'فريق',
    0x67: 'اختر الفريق',
    0x68: 'اختر فريق الحاسوب',
    0x69: 'اضغط \x08x للبدء',
    0x6A: 'خروج',
    # --- results --------------------------------------------------------------
    0x6B: 'ضربة قاضية',
    0x6C: 'ممتاز!',
    0x6D: 'الساحة',
    0x6E: 'تحطم!',
    0x6F: 'منافس جديد!',
    0x70: 'تم فتح مظهر جديد!',
    0x71: 'اكتمل!',
    0x72: 'تهانينا',
    0x73: 'تم فتح توم!',
    0x74: 'تم فتح جيري!',
    0x75: 'تم فتح سبايك!',
    0x76: 'تم فتح بوتش!',
    0x77: 'تم فتح نيبلز!',
    0x78: 'تم فتح البطة!',
    0x79: 'تم فتح تايك!',
    0x7A: 'تم فتح النسر!',
    0x7B: 'هرب جيري الوحش!',
    0x7C: 'هرب القط الآلي!',
    0x7D: 'تم فتح الأسد!',
    0x7E: 'انتهت اللعبة',
    0x7F: '\x08O إنهاء',
    0x80: '\x08X متابعة',
    0x81: '\x08X قتال مرة أخرى',
    0x82: 'متابعة؟',
    0x83: 'انتهى الوقت!',
    0x84: 'لقد فزت!',
    0x85: 'لقد خسرت!',
    0x86: 'فاز الحاسوب!',
    0x87: 'خسر الحاسوب!',
    # --- audio ----------------------------------------------------------------
    0x88: 'مستوى المؤثرات:',
    0x89: 'مستوى الموسيقى:',
    0x8A: 'متابعة',
    0x8B: 'إنهاء',
    0x8C: 'تم',
    0x8D: 'الصوت',
    0x8E: 'تأكيد الإنهاء',
    # --- arenas ---------------------------------------------------------------
    0x8F: 'المطبخ',
    0x90: 'الشاطئ',
    0x91: 'السفينة',
    0x92: 'مكب الخردة',
    0x93: 'الكوخ',
    0x94: 'البيت المسكون',
    0x95: 'قاعة الولائم',
    0x96: 'الغرب المتوحش',
    0x97: 'ناطحة السحاب',
    0x98: 'الجحيم',
    0x99: 'حلبة الملاكمة',
    0x9A: 'المختبر',
    0x9B: 'السوق',
    0x9C: 'اضغط زر البدء',
    # --- multiplayer setup ----------------------------------------------------
    0x9D: 'اللعب الجماعي',
    0x9E: 'معركة',
    0x9F: 'الكل ضد الكل',
    0xA0: '\x08o رجوع',
    0xA1: 'اللاعب 3',
    0xA2: 'اللاعب 4',
    0xA3: 'نوع\nالشخصية',
    0xA4: 'لاعب',
    0xA9: 'إيقاف',
    0xAA: 'الصوت',
    0xAE: 'الفريق أ',
    0xAF: 'الفريق ب',
    0xB0: 'الفريق ج',
    0xB1: 'الفريق د',
    0xB2: 'اضغط زر البدء للمتابعة',
    0xB3: '\x08D تحريك المؤشر',
    0xB4: '\x08X التالي',
    0xB5: '\x08S السابق',
    0xBA: 'الفريق أ',
    0xBB: 'الفريق ب',
    0xBC: 'الفريق ج',
    0xBD: 'الفريق د',
    0xBE: 'لا يمكن لنوعي الحاسوب والإيقاف\nالضغط على زر البدء للمتابعة',
    0xBF: 'يلزم لاعب بشري واحد على الأقل\nللعب الجماعي',
    0xC0: 'يلزم فريقان مختلفان على الأقل\nللعب الجماعي',
    0xC1: 'يلزم وجود شخصية واحدة على الأقل\nليقاتلها اللاعب البشري',
    0xC2: 'يلزم فريقان في كل منهما شخصيتان\nللعب بالتبديل',
    0xC3: 'يمكن لكل لاعب بشري التحكم في\nشخصيتين كحد أقصى\nفي اللعب بالتبديل',
    0xC4: 'لا يمكن للاعب البشري التحكم إلا في\nشخصيات الفريق نفسه\nفي اللعب بالتبديل',
    0xC5: 'الخاسرون!',
    # --- tournament -----------------------------------------------------------
    0xC6: 'ترتيب البطولة',
    0xC7: 'انتصارات',
    0xC8: 'الأول إلى ',
    0xC9: 'بطولة',
    0xCA: 'الأول إلى:',
    0xCB: 'ابدأ',
    0xCC: 'لعبة سريعة',
    0xCD: 'بطولة',
    0xCE: ' اختر المستوى',
    0xCF: '\x08x للإنهاء السريع',
    # --- save / quit ----------------------------------------------------------
    0xD3: 'تعذر تحميل اللعبة المحفوظة.',
    0xD5: 'تعذر حفظ اللعبة.\nهل تريد المحاولة مرة أخرى؟ سيتم\nاستبدال الحفظ القديم إن وجد.',
    0xD8: 'هل أنت متأكد أنك تريد الخروج؟',
    0xD9: '\x08Xنعم      \x08Oلا',
    0xDA: 'فاز بالبطولة!',
    0xDB: 'نتائج البطولة',
    0xDC: 'علامات اللاعبين:',
    # The two strings that carry a printf conversion. They work because shaping is a RUNTIME
    # pass over the line the game has already formatted -- offline shaping would have
    # reversed "%d" into "d%" long before the value existed.
    0xDD: 'أعد توصيل وحدة التحكم بالمنفذ %d\nواضغط زر البدء للمتابعة',
    0xDE: '\x08O إنهاء',
    0xDF: 'التوازن',
    0xE0: 'الموسيقى',
    0xE1: 'المؤثرات',
    0xE2: 'أعد توصيل وحدة التحكم\nواضغط زر البدء للمتابعة',
}

# The save-game messages the port already rewords (fe_menu's kSaveText: the retail text talks
# about "the Xbox hard disk", which is wrong on this port).  Same wording, in Arabic.
SAVE_STRINGS = {
    0xD0: 'لا توجد لعبة محفوظة لتوم وجيري\nعلى هذا الجهاز.\nهل تريد إنشاء حفظ جديد؟',
    0xD1: 'توجد لعبة محفوظة لتوم وجيري\nعلى هذا الجهاز.\nهل تريد تحميل اللعبة المحفوظة؟',
    0xD2: 'جار تحميل اللعبة المحفوظة. من فضلك\nلا تطفئ الجهاز.',
    0xD4: 'جار حفظ اللعبة. من فضلك\nلا تطفئ الجهاز.',
    0xD7: 'توجد بالفعل لعبة محفوظة لتوم وجيري\nعلى هذا الجهاز. هل تريد استبدالها\nبحفظ جديد؟ سيفقد الحفظ القديم.',
}

# The port's OWN rows, which are not in the retail table at all.  fe_menu serves these from
# custom indices and ArabicText answers before it, so they translate like any other string.
# 0xE3/0xE4 are built at runtime (they interpolate numbers) and are left alone for now.
# Rows this PORT added, which are not in the retail table at all.  ArabicText answers before
# each module's own provider, so they translate like any other string.
CUSTOM_STRINGS = {
    0xE5:  'هل أنت متأكد أنك تريد الخروج؟',   # fe_menu: the quit confirmation
    0x1F0: 'اللغة: العربية',                    # OPTIONS LANGUAGE row (English half in fe_menu)
    # The VIDEO screen. RESOLUTION interpolates numbers, so only its PREFIX is shaped here and
    # fe_menu composes "<w>X<h> :<prefix>" -- which is the correct VISUAL order, because a
    # Latin/digit run keeps its direction inside right-to-left text and therefore comes first
    # in the drawn stream. DISPLAY has three fixed states, so those are whole strings.
    0x1F1: 'الدقة:',      # fe_menu appends the numbers, in reading order
    0x1F3: 'العرض: نافذة',
    0x1F4: 'العرض: بلا إطار',
    0x1F5: 'العرض: ملء الشاشة',
    # meat_menu: the MULTIPLAYER row and its submenu
    0x1C8: 'اللعب الجماعي',
    0x1C9: 'لعبة سريعة',
    0x1CA: 'بطولة',
    0x1CB: 'سباق اللحم',
    # meat_ui: the two FIGHT SETTINGS rows the port added
    # MEAT RUSH row VALUES (meat_ui.cpp kValBase 0xE6). Untranslated they were English
    # capitals on an Arabic line, and the Arabic font has no Latin capitals -- so the row
    # drew its label and then nothing at all. The \x071 is retail's VALUE colour.
    0xEA: '\x071غير محدود',        # UNLIMITED (max meat)
    0xEB: '\x071ظاهرة',            # SCORES: SHOWN
    0xEC: '\x071مخفية',            # SCORES: HIDDEN
    0x1A0: 'أقصى لحم:',
    0x1A1: 'النتائج:',
}

ALL = dict(STRINGS)
ALL.update(SAVE_STRINGS)
ALL.update(CUSTOM_STRINGS)
