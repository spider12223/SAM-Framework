# Vanilla sounds

Every sound Barony ships, by the name a mod uses to replace it. Generated from the game's own
`sound/sounds.txt` -- 841 files in 363 groups.

**Replace one** by dropping a file with its name in your mod's `sounds/replace/` folder:
`sounds/replace/RatDie.ogg`. A **group** name replaces every variant at once:
`sounds/replace/SwingWeapon.ogg` changes all five weapon swings. The number beside each sound
works too. A few names are both a group and one sound in it (`Casting` is `Casting`, `Casting1V1`,
`Casting1V2` and `Casting2V1`). Such a name means the whole group; to change just the one sound,
use its number (`sounds/replace/170.ogg`).

**Search in game:** type `/sam_sounds vanilla <part of a name>` in the console, and
`/sam_playsound <name>` to hear it (through your replacement, if you made one).

## The ones people ask for first

| Name | What it is |
|---|---|
| `LeatherSteps` | footsteps (leather boots / bare feet) |
| `IronSteps` | footsteps (iron boots) |
| `SteelSteps` | footsteps (steel boots) |
| `SwingWeapon` | swinging a melee weapon |
| `Damage` | being hit |
| `Miss` | an attack that misses |
| `ArrowHit` | an arrow hitting |
| `ArrowRelease` | firing a bow |
| `BowDraw` | drawing a bow |
| `DoorOpen` | a door opening |
| `DoorClose1V2` | a door closing |
| `PickupItem` | picking an item up |
| `DropItem` | dropping an item |
| `GetGold` | picking up gold |
| `EquipWeapon` | equipping a weapon |
| `EquipArmor` | equipping armour |
| `WearJewelry` | putting on a ring or amulet |
| `Eat` | eating |
| `Casting` | casting a spell |
| `FailedSpell` | a spell fizzling |
| `Lever` | pulling a lever |
| `PageTurn` | turning a book page |
| `RatSpot` | a rat noticing you |
| `RatDie` | a rat dying |
| `GoblinSpot` | a goblin noticing you |
| `GoblinDie` | a goblin dying |

## Every sound, by group

| Group | Sounds (index: exact name) |
|---|---|
| `1` | 492: `1` |
| `10` | 604: `10` |
| `2` | 493: `2` |
| `3` | 494: `3` |
| `4` | 495: `4` |
| `5` | 496: `5` |
| `6` | 497: `6` |
| `7` | 498: `7` |
| `8` | 499: `8` |
| `9` | 500: `9` |
| `AccursedCure` | 402: `AccursedCure` |
| `AccursedStart` | 403: `AccursedStart` |
| `AcidHit` | 249: `AcidHit` |
| `Alchemy` | 566: `Alchemy1V1`, 567: `Alchemy2V1` |
| `Ambience` | 149: `Ambience-01` |
| `AmplifyMagic` | 420: `AmplifyMagic1V2` |
| `Appraise` | 535: `Appraise1V1`, 536: `Appraise2V1`, 537: `Appraise3V1`, 538: `Appraise4V1`, 539: `Appraise5V1`, 543: `Appraise1V2`, 544: `Appraise1V3`, 545: `Appraise1V4`, 546: `Appraise1V5`, 550: `Appraise1V6`, 568: `Appraise1V7`, 569: `Appraise2V2` |
| `ArrowHit` | 72: `ArrowHit-01`, 73: `ArrowHit-02`, 74: `ArrowHit-03` |
| `ArrowRelease` | 239: `ArrowRelease1V1`, 240: `ArrowRelease2V1`, 241: `ArrowRelease3V1`, 411: `ArrowRelease4V1`, 412: `ArrowRelease5V1`, 413: `ArrowRelease6V1` |
| `AutomatonAmbient` | 257: `AutomatonAmbient1V2`, 258: `AutomatonAmbient2V2`, 259: `AutomatonAmbient3V1` |
| `AutomatonCharge` | 321: `AutomatonCharge` |
| `AutomatonDie` | 260: `AutomatonDie1V1`, 261: `AutomatonDie2V1` |
| `AutomatonSpot` | 263: `AutomatonSpot1V2`, 264: `AutomatonSpot2V2`, 265: `AutomatonSpot3V2`, 778: `AutomatonSpot3V1` |
| `Axe` | 570: `Axe1V1`, 571: `Axe2V1` |
| `BadSpell` | 404: `BadSpell1V1` |
| `ball_impact` | 764: `ball_impact` |
| `BatDie` | 670: `BatDie1V1`, 671: `BatDie2V1` |
| `BatIdle` | 667: `BatIdle1V1`, 668: `BatIdle2V1`, 669: `BatIdle3V1` |
| `BatSpot` | 666: `BatSpot1V1` |
| `beartrapset` | 253: `beartrapset` |
| `bell_crash` | 660: `bell_crash`, 691: `bell_crash1`, 692: `bell_crash2` |
| `bell_muted` | 689: `bell_muted` |
| `bell_ring` | 659: `bell_ring` |
| `bell_ring_single` | 690: `bell_ring_single` |
| `BellyRumble` | 32: `BellyRumble` |
| `bolas` | 763: `bolas1` |
| `bolas_swing` | 765: `bolas_swing1`, 766: `bolas_swing2` |
| `bolas_throw` | 767: `bolas_throw`, 768: `bolas_throw2`, 769: `bolas_throw3` |
| `BoomerangCatch` | 431: `BoomerangCatch1V2`, 432: `BoomerangCatch2V2`, 433: `BoomerangCatch3V2` |
| `BoomerangFly` | 434: `BoomerangFly1V1`, 435: `BoomerangFly2V1`, 436: `BoomerangFly3V1`, 437: `BoomerangFly4V1`, 438: `BoomerangFly5V1`, 439: `BoomerangFly6V1`, 440: `BoomerangFly7V1`, 441: `BoomerangFly8V1`, 442: `BoomerangFly9V1`, 443: `BoomerangFly10V1` |
| `BoomerangThrow` | 427: `BoomerangThrow1V2`, 428: `BoomerangThrow2V2`, 429: `BoomerangThrow3V2`, 430: `BoomerangThrow4V3` |
| `Boulder` | 150: `Boulder` |
| `BoulderCrunch` | 181: `BoulderCrunch` |
| `BoulderRoll` | 151: `BoulderRoll` |
| `BoulderThump` | 182: `BoulderThump` |
| `Bow` | 75: `Bow` |
| `BowDraw` | 246: `BowDraw1V1`, 410: `BowDraw2V1` |
| `break_pottery` | 658: `break_pottery` |
| `breakablewood` | 661: `breakablewood3V2`, 662: `breakablewood4V1`, 663: `breakablewood5V1` |
| `breakbulb` | 664: `breakbulb` |
| `BreakDoor` | 177: `BreakDoor` |
| `BreakMetal` | 76: `BreakMetal` |
| `BreakWood` | 176: `BreakWood` |
| `Brew` | 401: `Brew` |
| `BugbearAmbient` | 673: `BugbearAmbient1V1`, 674: `BugbearAmbient2V1`, 675: `BugbearAmbient3V1` |
| `BugbearDie` | 676: `BugbearDie1V1`, 677: `BugbearDie2V1`, 678: `BugbearDie3V1` |
| `BugbearSpot` | 679: `BugbearSpot1V1`, 680: `BugbearSpot2V1` |
| `BugbearTaunt` | 681: `BugbearTaunt1V1`, 682: `BugbearTaunt2V1` |
| `BurnUp` | 512: `BurnUp` |
| `BustWall` | 67: `BustWall` |
| `Buy` | 89: `Buy` |
| `buzz` | 707: `buzz` |
| `Callout-neg` | 607: `Callout-neg4` |
| `Callout-neutral` | 605: `Callout-neutral4` |
| `Callout-pos` | 606: `Callout-pos4` |
| `Casting` | 170: `Casting`, 572: `Casting1V1`, 573: `Casting2V1`, 596: `Casting1V2` |
| `casting_interact` | 759: `casting_interact5`, 760: `casting_interact6`, 761: `casting_interact7`, 762: `casting_interact4`, 777: `casting_interact8` |
| `CHA` | 523: `CHA1V1` |
| `charge_mysticism` | 822: `charge_mysticism` |
| `CockatriceAmbient` | 382: `CockatriceAmbient1V2`, 383: `CockatriceAmbient2V2`, 384: `CockatriceAmbient2V2` |
| `CockatriceDeath` | 388: `CockatriceDeath1V2`, 389: `CockatriceDeath2V2`, 390: `CockatriceDeath2V2` |
| `CockatriceSpot` | 385: `CockatriceSpot1V2`, 386: `CockatriceSpot2V2`, 387: `CockatriceSpot3V2` |
| `CompendiumBack` | 634: `CompendiumBack1v1` |
| `CompendiumClose` | 635: `CompendiumClose1v1`, 636: `CompendiumClose2v1`, 637: `CompendiumClose3v1` |
| `CompendiumPageTurn` | 638: `CompendiumPageTurn1v1`, 639: `CompendiumPageTurn2v1`, 640: `CompendiumPageTurn3v1`, 641: `CompendiumPageTurn4v1`, 642: `CompendiumPageTurn5v1`, 643: `CompendiumPageTurn6v1`, 644: `CompendiumPageTurn7v1`, 645: `CompendiumPageTurn8v1` |
| `CompendiumReveal` | 632: `CompendiumReveal1V4`, 633: `CompendiumReveal1V4` |
| `CompendiumSort` | 646: `CompendiumSort1v1`, 647: `CompendiumSort2v1`, 648: `CompendiumSort3v1` |
| `CompendiumTab` | 649: `CompendiumTab4v1`, 650: `CompendiumTab4v1`, 651: `CompendiumTab4v1`, 652: `CompendiumTab4v2`, 653: `CompendiumTab4v2`, 654: `CompendiumTab4v2`, 655: `CompendiumTab4v3`, 656: `CompendiumTab4v3` |
| `CON` | 520: `CON1V1`, 531: `CON1V2` |
| `cooking` | 774: `cooking2V1`, 775: `cooking3V1` |
| `Crab` | 502: `Crab1V1`, 503: `Crab2V1`, 504: `Crab3V1`, 505: `Crab4V1`, 506: `Crab5V1`, 507: `Crab6V1`, 508: `Crab7V1`, 509: `Crab8V1`, 510: `Crab9V1` |
| `Craft` | 459: `Craft1V1`, 460: `Craft2V1`, 461: `Craft3V1` |
| `CrystalGolemAmbient` | 273: `CrystalGolemAmbient1V1`, 274: `CrystalGolemAmbient2V1`, 275: `CrystalGolemAmbient3V1` |
| `CrystalGolemDie` | 269: `CrystalGolemDie1V1`, 270: `CrystalGolemDie2V1`, 271: `CrystalGolemDie3V1`, 272: `CrystalGolemDie4V1` |
| `CrystalGolemSpot` | 266: `CrystalGolemSpot1V2`, 267: `CrystalGolemSpot2V2`, 268: `CrystalGolemSpot3V2` |
| `Damage` | 28: `Damage` |
| `Death` | 209: `Death` |
| `defy_flesh` | 823: `defy_flesh` |
| `Demon` | 210: `Demon1V1`, 211: `Demon2V1`, 212: `Demon3V1`, 213: `Demon4V1`, 214: `Demon5V1`, 215: `Demon6V1`, 216: `Demon7V1` |
| `DevilRoar` | 204: `DevilRoar-01`, 205: `DevilRoar-02`, 206: `DevilRoar-03`, 207: `DevilRoar-04`, 208: `DevilRoar-05` |
| `DevilSpeech` | 187: `DevilSpeech-01`, 188: `DevilSpeech-02`, 189: `DevilSpeech-03`, 190: `DevilSpeech-04`, 191: `DevilSpeech-05`, 192: `DevilSpeech-06`, 193: `DevilSpeech-07`, 194: `DevilSpeech-08`, 195: `DevilSpeech-09`, 196: `DevilSpeech-10` |
| `DEX` | 519: `DEX1V1` |
| `Ding` | 554: `Ding1V1`, 555: `Ding1V2`, 556: `Ding1V3`, 557: `Ding1V4` |
| `DoorClose` | 22: `DoorClose1V2` |
| `DoorOpen` | 21: `DoorOpen1V2wav`, 558: `DoorOpen1V1` |
| `Drink` | 52: `Drink1V1` |
| `DropItem` | 47: `DropItem1V1`, 48: `DropItem2V1`, 49: `DropItem3V1` |
| `DryadAmbient` | 720: `DryadAmbient1V4`, 721: `DryadAmbient1V5`, 722: `DryadAmbient1V6`, 831: `DryadAmbient1V4` |
| `DryadDie` | 723: `DryadDie1V3`, 724: `DryadDie1V4` |
| `DryadSpot` | 725: `DryadSpot1V3`, 726: `DryadSpot1V4`, 830: `DryadSpot1V3` |
| `duck_alert` | 784: `duck_alert_1V1`, 785: `duck_alert_1V2` |
| `duck_angry` | 786: `duck_angry_1v1`, 787: `duck_angry_1v2`, 788: `duck_angry_1v3` |
| `duck_content` | 789: `duck_content_1v1`, 790: `duck_content_1v2`, 791: `duck_content_1v3`, 792: `duck_content_1v4`, 793: `duck_content_1v5` |
| `duck_wings` | 794: `duck_wings_1v1`, 795: `duck_wings_1v2` |
| `duckwalk` | 779: `duckwalk_1V1`, 780: `duckwalk_1V2`, 781: `duckwalk_1V3`, 782: `duckwalk_1V4`, 783: `duckwalk_1V5` |
| `DummyIdle` | 416: `DummyIdle1V1` |
| `DummySpring` | 417: `DummySpring1V2`, 418: `DummySpring2V2`, 419: `DummySpring3V2` |
| `EarthShatter` | 799: `EarthShatter1V1` |
| `EarthSpriteAmbient` | 796: `EarthSpriteAmbient1V1`, 800: `EarthSpriteAmbient2V1`, 801: `EarthSpriteAmbient2V2`, 802: `EarthSpriteAmbient2V3` |
| `EarthSpriteDie` | 798: `EarthSpriteDie1V1` |
| `EarthSpriteSpot` | 797: `EarthSpriteSpot1V1` |
| `Eat` | 50: `Eat1V1`, 51: `Eat2V1` |
| `EquipArmor` | 44: `EquipArmor1V1`, 45: `EquipArmor2V1`, 46: `EquipArmor3V1` |
| `EquipWeapon` | 40: `EquipWeapon1V1`, 41: `EquipWeapon2V1`, 42: `EquipWeapon3V1`, 43: `EquipWeapon4V1` |
| `erudyce_angry` | 392: `erudyce_angryV1` |
| `erudyce_cackle` | 380: `erudyce_cackleV1` |
| `erudyce_caves` | 353: `erudyce_caves2V1`, 354: `erudyce_caves3V1` |
| `erudyce_caves1a` | 349: `erudyce_caves1aV1` |
| `erudyce_caves1b` | 351: `erudyce_caves1bV1` |
| `erudyce_citadel` | 358: `erudyce_citadel1V1`, 361: `erudyce_citadel2V1`, 362: `erudyce_citadel3V1` |
| `erudyce_combat` | 377: `erudyce_combat1V1`, 378: `erudyce_combat2V1`, 379: `erudyce_combat3V1` |
| `erudyce_death` | 381: `erudyce_deathV1` |
| `erudyce_endgame` | 365: `erudyce_endgameV1` |
| `erudyce_greeting` | 341: `erudyce_greeting1V1`, 344: `erudyce_greeting2V1`, 347: `erudyce_greeting3V1` |
| `erudyce_minotaur` | 366: `erudyce_minotaur1V1`, 369: `erudyce_minotaur2V1`, 370: `erudyce_minotaur3V1` |
| `erudyce_transition` | 356: `erudyce_transitionV1` |
| `Escape` | 180: `Escape` |
| `FailedSpell` | 163: `FailedSpell1V1`, 564: `FailedSpell1V2` |
| `FaucetBreak` | 132: `FaucetBreak1V1` |
| `fear` | 687: `fear` |
| `Fire` | 133: `Fire1V1`, 710: `Fire1V1` |
| `FireballExplode` | 153: `FireballExplode` |
| `flicker` | 708: `flicker` |
| `foci_arcs` | 817: `foci_arcs6`, 818: `foci_arcs7` |
| `foci_ice` | 813: `foci_ice2`, 816: `foci_ice` |
| `foci_needles` | 814: `foci_needles2` |
| `foci_sand` | 815: `foci_sand` |
| `Forcebolt` | 169: `Forcebolt` |
| `Freeze` | 197: `Freeze1V1` |
| `frypan_parry` | 776: `frypan_parry` |
| `gameover` | 511: `gameover` |
| `GateClose` | 82: `GateClose` |
| `GateOpen` | 81: `GateOpen` |
| `Generic` | 562: `Generic1V1` |
| `GetGold` | 38: `GetGold1V1`, 39: `GetGold2V1` |
| `ghostblink` | 608: `ghostblink1v1`, 609: `ghostblink1v2`, 610: `ghostblink1v3` |
| `ghostbounce` | 612: `ghostbounce1v1`, 613: `ghostbounce1v2`, 614: `ghostbounce1v3` |
| `GhoulDie` | 145: `GhoulDie-01` |
| `GhoulIdle` | 146: `GhoulIdle-01`, 147: `GhoulIdle-02`, 148: `GhoulIdle-03` |
| `GhoulSpot` | 142: `GhoulSpot-01`, 143: `GhoulSpot-02`, 144: `GhoulSpot-03` |
| `GlassSmash` | 162: `GlassSmash` |
| `Gnome` | 217: `Gnome1V1`, 218: `Gnome2V1`, 219: `Gnome3V1`, 220: `Gnome4V1`, 221: `Gnome5V1`, 222: `Gnome6V1`, 223: `Gnome7V1`, 224: `Gnome8V1`, 225: `Gnome9V1`, 226: `Gnome10V1`, 227: `Gnome11V1`, 228: `Gnome12V1`, 835: `Gnome1V1`, 836: `Gnome2V1`, 837: `Gnome3V1`, 838: `Gnome4V1`, 839: `Gnome5V1`, 840: `Gnome6V1`, 841: `Gnome7V1`, 842: `Gnome8V1` |
| `Gnome2` | 693: `Gnome2_4V1`, 694: `Gnome2_5V1`, 695: `Gnome2_6V1`, 696: `Gnome2_7V1`, 697: `Gnome2_8V1`, 698: `Gnome2_9V1`, 699: `Gnome2_10V1`, 700: `Gnome2_11V1`, 701: `Gnome2_12V1` |
| `GnomeWhisper` | 683: `GnomeWhisper1V1`, 684: `GnomeWhisper2V1`, 685: `GnomeWhisper3V1` |
| `GoatmanAmbient` | 332: `GoatmanAmbient1V1`, 333: `GoatmanAmbient2V1` |
| `GoatmanDie` | 338: `GoatmanDie1V1`, 339: `GoatmanDie2V1` |
| `GoatmanSpot` | 335: `GoatmanSpot1V1`, 336: `GoatmanSpot2V1`, 337: `GoatmanSpot3V1` |
| `GoblinDie` | 63: `GoblinDie-01`, 64: `GoblinDie-02`, 65: `GoblinDie-03` |
| `GoblinIdle` | 98: `GoblinIdle-01`, 99: `GoblinIdle-02`, 100: `GoblinIdle-03` |
| `GoblinSpot` | 60: `GoblinSpot-01`, 61: `GoblinSpot-02`, 62: `GoblinSpot-03` |
| `GoldB` | 242: `GoldB1V1`, 243: `GoldB2V1`, 244: `GoldB3V1`, 245: `GoldB4V1` |
| `GoodSpell` | 251: `GoodSpell1V1` |
| `GremlinAmbient` | 730: `GremlinAmbient1V1`, 731: `GremlinAmbient1V2`, 732: `GremlinAmbient1V3`, 843: `GremlinAmbient1V1`, 844: `GremlinAmbient1V2`, 845: `GremlinAmbient1V3` |
| `GremlinDie` | 733: `GremlinDie1V1`, 734: `GremlinDie1V2`, 735: `GremlinDie1V3` |
| `GremlinSpot` | 736: `GremlinSpot1V1`, 737: `GremlinSpot1V2`, 738: `GremlinSpot1V3` |
| `GyroDanger` | 450: `GyroDanger1V3` |
| `GyroIdle` | 444: `GyroIdle1V1`, 445: `GyroIdle2V1`, 446: `GyroIdle3V1`, 447: `GyroIdle4V1` |
| `GyroItem` | 448: `GyroItem1V4` |
| `GyroReturn` | 449: `GyroReturn1V3` |
| `GyroStart` | 415: `GyroStart1V3` |
| `Healing` | 168: `Healing` |
| `heat_down` | 826: `heat_down` |
| `heat_up` | 827: `heat_up` |
| `Herx-Darkness` | 179: `Herx-Darkness` |
| `Herx-GoodLuck` | 117: `Herx-GoodLuck-01`, 118: `Herx-GoodLuck-02`, 119: `Herx-GoodLuck-03` |
| `Herx-Greeting` | 185: `Herx-Greeting01` |
| `Herx-Labyrinth` | 158: `Herx-Labyrinth01`, 159: `Herx-Labyrinth02` |
| `Herx-Laugh` | 120: `Herx-Laugh-01`, 121: `Herx-Laugh-02`, 122: `Herx-Laugh-03` |
| `Herx-Minotaur` | 123: `Herx-Minotaur-01`, 124: `Herx-Minotaur-02`, 125: `Herx-Minotaur-03` |
| `Herx-New` | 126: `Herx-New-01`, 127: `Herx-New-02`, 128: `Herx-New-03` |
| `Herx-No` | 186: `Herx-No` |
| `Herx-Ruins` | 160: `Herx-Ruins01`, 161: `Herx-Ruins02` |
| `Herx-StickAround` | 129: `Herx-StickAround-01`, 130: `Herx-StickAround-02`, 131: `Herx-StickAround-03` |
| `Herx-Swamp` | 156: `Herx-Swamp01`, 157: `Herx-Swamp02` |
| `hit_pottery` | 657: `hit_pottery` |
| `Identify` | 167: `Identify` |
| `IgniteTorch` | 134: `IgniteTorch1V1` |
| `Imp` | 198: `Imp-01`, 199: `Imp-02`, 200: `Imp-03`, 201: `Imp-04`, 202: `Imp-05`, 203: `Imp-06` |
| `incoherence` | 825: `incoherence` |
| `IncubusAmbient` | 276: `IncubusAmbient1V2`, 277: `IncubusAmbient2V2`, 278: `IncubusAmbient3V2` |
| `IncubusDie` | 279: `IncubusDie1V2`, 280: `IncubusDie2V2`, 281: `IncubusDie3V2` |
| `IncubusSpot` | 282: `IncubusSpot1V2`, 283: `IncubusSpot2V2`, 284: `IncubusSpot3V2` |
| `InsectoidAmbient` | 285: `InsectoidAmbient1V1`, 286: `InsectoidAmbient2V1` |
| `InsectoidDie` | 287: `InsectoidDie1V1`, 288: `InsectoidDie2V1`, 289: `InsectoidDie3V1`, 290: `InsectoidDie4V1` |
| `InsectoidSpot` | 291: `InsectoidSpot1V1`, 292: `InsectoidSpot2V1`, 293: `InsectoidSpot3V1`, 294: `InsectoidSpot4V1` |
| `INT` | 521: `INT1V1`, 532: `INT1V2`, 533: `INT1V3`, 534: `INT1V4`, 551: `INT1V5`, 552: `INT1V6`, 553: `INT1V7` |
| `Invisible` | 166: `Invisible` |
| `IronSteps` | 7: `IronSteps-01`, 8: `IronSteps-02`, 9: `IronSteps-03`, 10: `IronSteps-04`, 11: `IronSteps-05`, 12: `IronSteps-06`, 13: `IronSteps-07` |
| `ItemClatter` | 53: `ItemClatter1V1`, 54: `ItemClatter2V1`, 55: `ItemClatter3V1` |
| `KeyUse` | 702: `KeyUse1V1`, 703: `KeyUse2V1`, 704: `KeyUse3V1`, 705: `KeyUse4V1`, 706: `KeyUse5V1` |
| `KoboldAmbient` | 295: `KoboldAmbient1V2`, 296: `KoboldAmbient2V2` |
| `KoboldDie` | 298: `KoboldDie1V1`, 299: `KoboldDie2V1`, 300: `KoboldDie3V1`, 301: `KoboldDie4V1` |
| `KoboldSpot` | 302: `KoboldSpot1V2`, 303: `KoboldSpot2V2` |
| `Ladder` | 96: `Ladder` |
| `Lava` | 155: `Lava` |
| `Leader` | 574: `Leader1V1`, 601: `Leader1V2`, 602: `Leader1V3`, 603: `Leader1V4` |
| `LeatherSteps` | 0: `LeatherSteps-01`, 1: `LeatherSteps-02`, 2: `LeatherSteps-03`, 3: `LeatherSteps-04`, 4: `LeatherSteps-05`, 5: `LeatherSteps-06`, 6: `LeatherSteps-07` |
| `leaves` | 752: `leaves1`, 753: `leaves2`, 754: `leaves3`, 755: `leaves4`, 828: `leaves1`, 829: `leaves2` |
| `LevelUp` | 97: `LevelUp` |
| `Levelup` | 524: `Levelup1V2`, 525: `Levelup2V1`, 526: `Levelup3V1`, 527: `Levelup4V1`, 528: `Levelup5V1` |
| `Lever` | 56: `Lever1V1`, 57: `Lever2V1`, 58: `Lever3V1` |
| `LeverTimerCreak` | 248: `LeverTimerCreak` |
| `Levitate` | 178: `Levitate` |
| `LightBurst` | 165: `LightBurst` |
| `LightningHit` | 173: `LightningHit` |
| `LightningImpact` | 517: `LightningImpact` |
| `LockedDoor` | 152: `LockedDoor` |
| `Mace` | 575: `Mace1V1` |
| `Magic` | 576: `Magic1V1`, 577: `Magic2V1`, 597: `Magic1V2` |
| `Magictrap` | 393: `Magictrap1V1`, 394: `Magictrap2V1`, 395: `Magictrap3V1` |
| `magictraptrigger` | 252: `magictraptrigger` |
| `MenuEnter` | 137: `MenuEnter1V1` |
| `MenuReturn` | 138: `MenuReturn1V1` |
| `MenuSelect` | 139: `MenuSelect1V1` |
| `meteor` | 820: `meteor1` |
| `meteor_impact` | 819: `meteor_impact` |
| `MimicAlert` | 619: `MimicAlert1V1`, 620: `MimicAlert2V1`, 621: `MimicAlert3V1` |
| `MimicAttack` | 622: `MimicAttack1V1`, 623: `MimicAttack2V1`, 624: `MimicAttack3V1` |
| `MimicDeath` | 625: `MimicDeath1V1`, 626: `MimicDeath2V1`, 627: `MimicDeath3V1` |
| `MimicShuffle` | 628: `MimicShuffle1V1`, 629: `MimicShuffle2V1`, 630: `MimicShuffle3V1`, 631: `MimicShuffle4V1` |
| `MimicWalk` | 615: `MimicWalk1V1`, 616: `MimicWalk2V1`, 617: `MimicWalk3V1`, 618: `MimicWalk4V1` |
| `MinimapPing` | 399: `MinimapPing` |
| `MinotaurAttack` | 113: `MinotaurAttack` |
| `MinotaurDie` | 114: `MinotaurDie` |
| `MinotaurIdle` | 110: `MinotaurIdle-01`, 111: `MinotaurIdle-02`, 112: `MinotaurIdle-03` |
| `MinotaurIntro` | 514: `MinotaurIntro1`, 515: `MinotaurIntro2`, 516: `MinotaurIntro3` |
| `MinotaurSpot` | 107: `MinotaurSpot-01`, 108: `MinotaurSpot-02`, 109: `MinotaurSpot-03` |
| `MinotaurWalk` | 115: `MinotaurWalk` |
| `Miss` | 31: `Miss` |
| `mushroom_charge` | 711: `mushroom_charge1`, 712: `mushroom_charge2`, 713: `mushroom_charge3` |
| `mushroom_charge_short` | 714: `mushroom_charge_short1`, 715: `mushroom_charge_short2`, 716: `mushroom_charge_short3` |
| `mushroom_spell` | 717: `mushroom_spell1`, 718: `mushroom_spell2`, 719: `mushroom_spell3` |
| `MyconidAmbient` | 742: `MyconidAmbient1V1`, 743: `MyconidAmbient1V2`, 744: `MyconidAmbient1V3`, 832: `MyconidAmbient1V1`, 833: `MyconidAmbient1V2`, 834: `MyconidAmbient1V3` |
| `MyconidDie` | 745: `MyconidDie1V1`, 746: `MyconidDie1V2` |
| `MyconidSpot` | 747: `MyconidSpot1V1`, 748: `MyconidSpot1V2` |
| `NewSpell` | 560: `NewSpell1V4` |
| `No` | 90: `No` |
| `Noisemaker` | 455: `Noisemaker1V1` |
| `NoiseMakerEnd` | 485: `NoiseMakerEnd1V1`, 486: `NoiseMakerEnd2V1`, 487: `NoiseMakerEnd3V1` |
| `NoiseMakerSfx` | 472: `NoiseMakerSfx1V1`, 473: `NoiseMakerSfx2V1`, 474: `NoiseMakerSfx3V1`, 475: `NoiseMakerSfx4V1`, 476: `NoiseMakerSfx5V1`, 477: `NoiseMakerSfx6V1`, 478: `NoiseMakerSfx7V1`, 479: `NoiseMakerSfx8V1`, 480: `NoiseMakerSfx9V1`, 481: `NoiseMakerSfx10V1`, 482: `NoiseMakerSfx11V1`, 483: `NoiseMakerSfx12V1`, 484: `NoiseMakerSfx13V1` |
| `NoMana` | 563: `NoMana1V1` |
| `orpheus_angry` | 391: `orpheus_angryV1` |
| `orpheus_cackle` | 375: `orpheus_cackleV1` |
| `orpheus_caves` | 350: `orpheus_caves1V1`, 352: `orpheus_caves2V1`, 355: `orpheus_caves3V1` |
| `orpheus_citadel` | 359: `orpheus_citadel1V1`, 360: `orpheus_citadel2V1`, 363: `orpheus_citadel3V1` |
| `orpheus_combat` | 372: `orpheus_combat1V1`, 373: `orpheus_combat2V1`, 374: `orpheus_combat3V1` |
| `orpheus_death` | 376: `orpheus_deathV1` |
| `orpheus_endgame` | 364: `orpheus_endgameV1` |
| `orpheus_greeting` | 342: `orpheus_greeting1V1` |
| `orpheus_greeting2a` | 343: `orpheus_greeting2aV1` |
| `orpheus_greeting2b` | 345: `orpheus_greeting2bV1` |
| `orpheus_greeting3a` | 346: `orpheus_greeting3aV1` |
| `orpheus_greeting3b` | 348: `orpheus_greeting3bV1` |
| `orpheus_minotaur` | 367: `orpheus_minotaur1V1`, 368: `orpheus_minotaur2V1`, 371: `orpheus_minotaur3V1` |
| `orpheus_transition` | 357: `orpheus_transitionV1` |
| `PageTurn` | 83: `PageTurn-01`, 84: `PageTurn-02`, 85: `PageTurn-03`, 86: `PageTurn-04`, 87: `PageTurn-05`, 88: `PageTurn-06` |
| `PedestalRise` | 250: `PedestalRise` |
| `PER` | 522: `PER1V1`, 529: `PER1V2` |
| `PickupItem` | 35: `PickupItem1V1`, 36: `PickupItem2V1`, 37: `PickupItem3V1` |
| `pinpoint_target` | 849: `pinpoint_target` |
| `PipeGroan` | 140: `PipeGroan1V1`, 141: `PipeGroan2V1` |
| `Polearm` | 578: `Polearm1V1` |
| `Polymorph` | 400: `Polymorph` |
| `PolymorphEnd` | 611: `PolymorphEnd` |
| `Portal` | 154: `Portal` |
| `psychic_spear` | 821: `psychic_spear` |
| `Puke` | 78: `Puke` |
| `Punch` | 183: `Punch`, 184: `Punch` |
| `Radio` | 238: `Radio` |
| `Ranged` | 579: `Ranged1V1`, 580: `Ranged2V1`, 600: `Ranged1V2` |
| `RatDie` | 30: `RatDie` |
| `RatSpot` | 29: `RatSpot` |
| `Recycle` | 424: `Recycle1V1` |
| `RobotGear` | 456: `RobotGear1V1`, 457: `RobotGear2V1`, 458: `RobotGear3V1` |
| `rope_pull` | 688: `rope_pull` |
| `RunningWater` | 135: `RunningWater1V1`, 672: `RunningWater1V1` |
| `SalamanderAmbient` | 850: `SalamanderAmbient1V3`, 851: `SalamanderAmbient2V3`, 852: `SalamanderAmbient3V3` |
| `SalamanderDie` | 856: `SalamanderDie1V3`, 857: `SalamanderDie2V3`, 858: `SalamanderDie3V3`, 859: `SalamanderDie4V3` |
| `SalamanderSpot` | 846: `SalamanderSpot2V3`, 847: `SalamanderSpot2V3`, 848: `SalamanderSpot3V3`, 853: `SalamanderSpot1V3`, 854: `SalamanderSpot2V3`, 855: `SalamanderSpot3V3` |
| `Salvage` | 462: `Salvage2V1`, 463: `Salvage3V1` |
| `ScarabAmbient` | 306: `ScarabAmbient2V2`, 307: `ScarabAmbient3V2` |
| `ScarabDie` | 308: `ScarabDie1V2`, 309: `ScarabDie2V2` |
| `ScarabSpot` | 310: `ScarabSpot1V2`, 311: `ScarabSpot2V2`, 312: `ScarabSpot3V2` |
| `ScorpionDie` | 104: `ScorpionDie-01`, 105: `ScorpionDie-02`, 106: `ScorpionDie-03` |
| `ScorpionSpot` | 101: `ScorpionSpot-01`, 102: `ScorpionSpot-02`, 103: `ScorpionSpot-03` |
| `SecretSound` | 488: `SecretSound1`, 489: `SecretSound2`, 490: `SecretSound3`, 491: `SecretSound4` |
| `SentryDie` | 451: `SentryDie1V1`, 452: `SentryDie2V1` |
| `SentryReturn` | 469: `SentryReturn1V1`, 470: `SentryReturn2V1`, 471: `SentryReturn3V1` |
| `SentrySpotA` | 464: `SentrySpotA1V1`, 465: `SentrySpotA2V1` |
| `SentrySpotB` | 466: `SentrySpotB1V2`, 467: `SentrySpotB2V2`, 468: `SentrySpotB3V2` |
| `SentryStart` | 453: `SentryStart1V1`, 454: `SentryStart2V1` |
| `ShadowAmbient` | 313: `ShadowAmbient1V1`, 314: `ShadowAmbient2V1`, 315: `ShadowAmbient3V1` |
| `ShadowDie` | 316: `ShadowDie1V1`, 317: `ShadowDie2V1` |
| `ShadowSpot` | 318: `ShadowSpot1V1`, 319: `ShadowSpot2V1` |
| `Shield` | 581: `Shield1V1`, 582: `Shield2V1`, 598: `Shield1V2`, 599: `Shield1V3` |
| `ShootColdball` | 172: `ShootColdball` |
| `ShootFireball` | 164: `ShootFireball` |
| `ShootLightning` | 171: `ShootLightning` |
| `SkeletonDie` | 94: `SkeletonDie` |
| `SkeletonSpot` | 93: `SkeletonSpot` |
| `SkeletonStep` | 95: `SkeletonStep-01` |
| `Skillup_Generic` | 559: `Skillup_Generic1V1` |
| `slam` | 709: `slam`, 807: `slam1V1` |
| `SlimeDeath` | 69: `SlimeDeath` |
| `SlimeSpot` | 68: `SlimeSpot` |
| `Slow` | 396: `Slow1V1`, 397: `Slow2V1`, 398: `Slow3V1` |
| `Sneak` | 540: `Sneak1V1`, 541: `Sneak2V1`, 542: `Sneak1V2`, 547: `Sneak1V3`, 548: `Sneak1V4`, 549: `Sneak1V5` |
| `Sonar` | 116: `Sonar` |
| `SpecialAttack` | 405: `SpecialAttack2V1` |
| `SpellbookCrumble` | 414: `SpellbookCrumble1V2` |
| `Spider` | 229: `Spider1V1`, 230: `Spider2V1`, 231: `Spider3V1`, 232: `Spider4V1`, 233: `Spider5V1`, 234: `Spider6V1`, 235: `Spider7V1`, 236: `Spider8V1`, 237: `Spider9V1` |
| `Splash` | 136: `Splash1V1` |
| `SprayWeb` | 425: `SprayWeb1V1`, 426: `SprayWeb2V1` |
| `Stealth` | 583: `Stealth1V1`, 584: `Stealth2V1` |
| `SteelSteps` | 14: `SteelSteps-01`, 15: `SteelSteps-02`, 16: `SteelSteps-03`, 17: `SteelSteps-04`, 18: `SteelSteps-05`, 19: `SteelSteps-06`, 20: `SteelSteps-07` |
| `StoryMusic` | 501: `StoryMusicV3` |
| `STR` | 518: `STR1V1`, 530: `STR1V2` |
| `StrikeWall` | 66: `StrikeWall` |
| `SuccubusDies` | 71: `SuccubusDies` |
| `SuccubusSpot` | 70: `SuccubusSpot` |
| `Swimming` | 585: `Swimming1V1`, 586: `Swimming2V1` |
| `SwingWeapon` | 23: `SwingWeapon1V1`, 24: `SwingWeapon2V1`, 25: `SwingWeapon3V1`, 26: `SwingWeapon4V1`, 27: `SwingWeapon5V1` |
| `SwingWeaponLow` | 770: `SwingWeaponLow1V1`, 771: `SwingWeaponLow2V1`, 772: `SwingWeaponLow3V1`, 773: `SwingWeaponLow4V1` |
| `Sword` | 587: `Sword1V1`, 588: `Sword2V1` |
| `Teleport` | 77: `Teleport` |
| `thunder` | 803: `thunder`, 804: `thunder2`, 805: `thunder3`, 806: `thunder4` |
| `Tick` | 247: `Tick` |
| `Tinkering` | 589: `Tinkering1V1`, 590: `Tinkering2V1` |
| `TinkeringRepair` | 421: `TinkeringRepair1V1`, 422: `TinkeringRepair2V1`, 423: `TinkeringRepair3V1` |
| `TinkerLevel` | 561: `TinkerLevel2V1` |
| `tinkertrapset` | 686: `tinkertrapset` |
| `Trading` | 591: `Trading1V1`, 592: `Trading2V1` |
| `TrollDie` | 80: `TrollDie` |
| `TrollSpot` | 79: `TrollSpot` |
| `Unarmed` | 593: `Unarmed1V1`, 594: `Unarmed2V1` |
| `UnlockDoor` | 91: `UnlockDoor` |
| `UnlockFailed` | 92: `UnlockFailed` |
| `VampireAmbient` | 322: `VampireAmbient1V1`, 323: `VampireAmbient2V1`, 324: `VampireAmbient3V1` |
| `VampireDie` | 325: `VampireDie1V1`, 326: `VampireDie2V1`, 327: `VampireDie3V1`, 328: `VampireDie4V1` |
| `VampireSpot` | 329: `VampireSpot1V1`, 330: `VampireSpot2V1`, 331: `VampireSpot3V1` |
| `WaterSplash` | 665: `WaterSplash` |
| `weakness` | 824: `weakness2` |
| `WeakwallStrike` | 565: `WeakwallStrike1V1` |
| `WearJewelry` | 33: `WearJewelry1V1`, 34: `WearJewelry2V1` |
| `WebwallStrike` | 595: `WebwallStrike1V2` |
| `WeirdSpell` | 174: `WeirdSpell` |
| `Whip` | 407: `Whip1V1`, 408: `Whip2V1`, 409: `Whip3V1` |
| `WhipDisarm` | 406: `WhipDisarm1V1` |
| `wind` | 756: `wind1`, 757: `wind2`, 758: `wind3` |
| `WoodCreak` | 513: `WoodCreak1V1` |
| `Zap` | 59: `Zap1V1` |
| `zap` | 808: `zap1`, 809: `zap2`, 810: `zap3`, 811: `zap4`, 812: `zap5` |
| `ZapBrigade` | 175: `ZapBrigade` |
