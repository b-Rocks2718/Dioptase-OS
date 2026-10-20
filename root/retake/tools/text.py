"""Every word the game shows on an intertitle card.

A script is a list of cards; a card is a string that build.py wraps to the
card width with the serif font. A blank line inside a card ("\n\n") forces a
paragraph break. Keep cards short: silent-film titles were read in seconds.
Script names become SCRIPT_<NAME> in pak.h.
"""

SCRIPTS = {
    "intro": [
        "Hollywood, 1937.",
        "Silverlake Pictures was shooting the most ambitious film ever made:\n\nTHE EVERLASTING PICTURE.",
        "Its director, Augustin Vale, had built a camera that could film a moment so perfectly...\n\n...that the moment would never have to end.",
        "On the night of the first screening, the camera kept rolling.",
        "It filmed the cast. It filmed the crew.\n\nIt filmed the whole studio lot, and it never stopped.",
        "Only one person on the lot was not in the picture.",
        "RUBY.\n\nStunt double. Uncredited.",
        "Nobody had ever written her name down.\n\nSo the camera never caught her.",
    
    ],
    "credits_tease": [
        "STUNTS: RUBY",
        "A game by\n\nCLAUDE OPUS\n\nand\n\nGPT-6 ASTRA",
    ],
    "death0": ["CUT!\n\nTake it from the top."],
    "death1": ["CUT!\n\nAgain. And this time, with feeling."],
    "death2": ["CUT!\n\nThe stunt department will hear about this."],
    "death3": ["CUT!\n\nReset the set. Places, everyone."],
    "saved": ["The mirror remembers your face.\n\n(Progress saved.)"],
    # ---- the Backlot ----
    "backlot_gate": [
        "SILVERLAKE PICTURES\n\nThe lot is silent. The searchlights still sweep for a premiere that ended long ago.",
        "Somewhere on every sound stage, a picture is still being shot.\n\nWrap each one, and maybe the studio lets its people go.",
    ],
    "stage_sealed": [
        "The stage door is sealed with silver nitrate.\n\nThis picture is still rolling inside, but you can't get in yet.",
    ],
    "tower_sealed": [
        "THE PROJECTION TOWER\n\nThe door has no handle. Only a slot, shaped like a reel of film.",
    ],
    "stage1_door": [
        "STAGE 1: \"SAFETY LAST STREET\"\n\nA slapstick comedy in two reels. Starring nobody you'd recognize.",
    ],
    # ---- Stage 1 ----
    "s1_title": [
        "SAFETY LAST STREET\n\nScene 1, Take 1.",
    ],
    "s1_mark_hint": [
        "STAGE MANAGER'S NOTE:\n\nEvery actor must hit their MARK.\nThe set won't move until somebody's standing on it.\n\n(C: start recording yourself. C again: cut.)",
    ],
    "s1_double_hint": [
        "STUNT COORDINATOR'S NOTE:\n\nCan't reach it? Two stunt doubles are better than one.\n\n(C: record yourself.)",
    ],
    "s1_wall_hint": [
        "RIGGING CREW:\n\nWall kicks only. Mind the sandbags.\n\n(Jump while sliding down a wall.)",
    ],
    "s1_clap_hint": [
        "PROPS:\n\nOne clapperboard, slightly haunted.\n\n(X: clap. Hold up or down to clap that way.)",
    ],
    "s1_comedian": [
        "\"Oh! A real person! In color, no less.\"",
        "\"I've slipped on this banana peel four thousand and six times. Every time, the audience roars.\"",
        "\"There isn't any audience, kid. There's just the camera.\"",
        "\"The Sergeant's up in the clock tower. Whatever you do, don't let him chase you in a straight line.\"",
    ],
    "s1_reel_a": [
        "LOST REEL: \"A Studio Is Born\" (1921)",
        "Silverlake Pictures began in an orange grove with one camera, one stage, and a director nobody wanted to hire.",
        "His name was Augustin Vale. By 1930, every studio in town wanted him.\n\nHe never left Silverlake.",
    ],
    "s1_reel_b": [
        "LOST REEL: \"The Lark\" (1929)",
        "Vivian Lark could cry on cue, fence like a musketeer, and sing in four languages.",
        "She could not fall off a horse, a building, or a moving train.\n\nThat was somebody else's job.",
    ],
    "s1_reel_c": [
        "LOST REEL: \"Test Footage, Camera No. 7\" (1936)",
        "A rabbit on a stool. The rabbit hops off.\n\nThe film keeps showing the rabbit on the stool.",
        "A handwritten note is taped to the can:\n\n\"IT REMEMBERS WHAT WE TELL IT TO.\n- A.V.\"",
    ],
    "s1_boss_intro": [
        "SERGEANT GEARHART\nThe Clockwork Constable",
        "\"HALT! NO ONE LEAVES THE PICTURE!\"",
    ],
    "s1_wrap": [
        "THAT'S A WRAP!\n\nStage 1 falls quiet. Somewhere below, a comedian finally stops slipping.",
        "Tangled in the Sergeant's springs: a coil of STUNT WIRE.\n\n(Press jump again in the air for a second jump.)",
    ],
    # ---- Stage 2 ----
    "s2_title": [
        "DUST & IRON\n\nA Western in three reels. Scene 1, Take 1.",
    ],
    "s2_sheriff": [
        "\"This town ain't big enough for the... for the...\"",
        "\"Dang it. Forty years I've been saying that line, and I still can't remember how it ends.\"",
        "\"You look like a gal who's read the script. The Iron Horse runs the yard past the mine. Mind the BRAKE: hold it down and that engine stops cold.\"",
    ],
    "s2_cart_hint": [
        "MINE FOREMAN:\n\nRide the carts. Don't ride the bulls.",
    ],
    "s2_brake_hint": [
        "SWITCHMAN'S NOTE:\n\nPull the BRAKE and the engine stops. Nobody can hold it and fight at the same time...",
        "...unless they happen to have a double.",
    ],
    "s2_reel_a": [
        "LOST REEL: \"Dust & Iron, Dailies\" (1934)",
        "Take 31. The stunt rider leaps from the cliff onto the moving train. Perfect.",
        "Vale's voice, off camera:\n\n\"Again. I want to see it a thousand times.\"",
    ],
    "s2_reel_b": [
        "LOST REEL: \"A Letter, Unsent\" (1935)",
        "\"Dear Augustin. The crew is tired. You keep asking for more takes than we have days.\"",
        "\"Pictures end. That is what makes them pictures.\n\n- V. L.\"",
    ],
    "s2_reel_c": [
        "LOST REEL: \"Camera No. 7, Lens Test\" (1936)",
        "The camera points at a mirror. In the mirror, the camera is pointed at a mirror.",
        "The film is longer than the reel it was wound on.",
    ],
    "s2_boss_intro": [
        "THE IRON HORSE",
        "Somewhere a whistle screams. The rails begin to hum.",
    ],
    "s2_wrap": [
        "THAT'S A WRAP!\n\nThe locomotive coughs, sighs, and rolls to a stop for good.",
        "In the cab, wrapped in a red flag: a pair of editor's shears.\n\nJUMP CUT. (Press V to cut straight to your newest take.)",
    ],
    # ---- Stage 3 ----
    "s3_title": [
        "THE CRIMSON TIDE\n\nA swashbuckler in four reels. Shot at night, on a sea made of canvas.",
    ],
    "s3_captain": [
        "\"AVAST! Captain Blackwater, terror of the seven seas!\"",
        "\"...also the terror of the props department. I owe them a ship.\"",
        "\"The Kraken's two stagehands and a rubber hose, lass. But down here, the hose bites.\"",
        "\"Strike the tentacles when they come down. They can't hurt you while they're stuck in the deck.\"",
    ],
    "s3_rigging_hint": [
        "BOATSWAIN:\n\nUp the ratlines, down the other side. Rope rungs hold you from above only.",
    ],
    "s3_cut_hint": [
        "EDITOR'S NOTE, scrawled on a crate:\n\nA story can skip the boring part. So can you.\n\n(V: cut to your latest take.)",
    ],
    "s3_landing_hint": [
        "STUNT COORDINATOR'S NOTE:\n\nThe audience never sees the landing.",
    ],
    "s3_reel_a": [
        "LOST REEL: \"Storyboards\" (1936)",
        "Sketches for The Everlasting Picture. Every panel shows the same scene: the cast, bowing, as the curtain refuses to fall.",
    ],
    "s3_reel_b": [
        "LOST REEL: \"Rehearsal\" (1937)",
        "Vivian, in costume, steps out of frame.\n\nThe film shows her stepping out of frame. Then stepping back in. Then out again.",
        "She is still doing it.",
    ],
    "s3_reel_c": [
        "LOST REEL: \"Payroll, Stunt Department\" (1937)",
        "A ledger page. Every stunt in every picture, signed for by one name.\n\nThe name has been crossed out so many times the paper tore.",
    ],
    "s3_boss_intro": [
        "THE KRAKEN",
        "(Two stagehands, forty feet of rubber hose, and a great deal of enthusiasm.)",
    ],
    "s3_wrap": [
        "THAT'S A WRAP!\n\nThe tentacles go limp. Somewhere under the deck, two stagehands sigh with relief.",
        "Tangled in the rigging: a battered black UMBRELLA.\n\n(Hold jump while falling to drift.)",
    ],
    # ---- Stage 4 ----
    "s4_title": [
        "JUNGLE SERIAL, CHAPTER 12\n\nPreviously: our heroine dangled over a pit of crocodiles. Now: everything else.",
    ],
    "s4_narrator": [
        "\"WILL OUR HEROINE ESCAPE THE TEMPLE OF THE APE KING?\"",
        "\"...sorry. Habit. I've been the voice of this serial for so long, I narrate everything.\"",
        "\"The ape's on top of the ziggurat. He throws boulders. He leaps. And he pounds his chest before every shockwave, so watch for that.\"",
        "\"Find out what happens next... in Chapter 13!\"",
    ],
    "s4_crumble_hint": [
        "EXPEDITION LOG, DAY 40:\n\nThe old stones crumble if you stand on them too long. Keep moving, or let somebody else stand there for you.",
    ],
    "s4_gate_hint": [
        "CARVED ON THE TEMPLE DOOR:\n\nONE MUST KNEEL FOR THE DOOR TO RISE. THE OTHER MAY WALK THROUGH.",
    ],
    "s4_reel_a": [
        "LOST REEL: \"Cliffhanger\" (1932)",
        "Chapter 11 ends with the heroine falling into a pit.\n\nChapter 12 opens with her already out of it. Nobody filmed how.",
        "Pencilled in the margin of the script:\n\n\"Stunt double does the fall. Cut before she lands.\"",
    ],
    "s4_reel_b": [
        "LOST REEL: \"Camera No. 7, Night Shoot\" (1937)",
        "The camera films an empty stage for one hour.\n\nOn the last frame, every seat in the house is full.",
    ],
    "s4_reel_c": [
        "LOST REEL: \"Vale, Interview\" (1935)",
        "\"Why make pictures?\"",
        "\"Because everyone I ever loved was a moment. I would like, just once, for a moment to stay.\"",
    ],
    "s4_boss_intro": [
        "GARGANTUA, KING OF THE APES",
        "(A man in a very hot suit, who has been doing this since 1931 and would like to sit down.)",
    ],
    "s4_wrap": [
        "THAT'S A WRAP!\n\nThe ape sits down at last. Somewhere, a zipper unzips with a sigh of relief.",
        "Behind the idol: a spare film magazine.\n\nA THIRD TAKE REEL. (You can keep three takes rolling at once.)",
    ],
    # ---- Stage 5 ----
    "s5_title": [
        "THE MONSTER OF HOLLOW MANOR\n\nA photoplay of terror. The sets lean. The shadows are painted on.",
    ],
    "s5_igor": [
        "\"Master? Master, is that you? ...No. Too short. Too colorful.\"",
        "\"The master built the creature from spare extras and a lightning rod. Then the camera filmed it, and now it won't stop rising from the slab.\"",
        "\"It calls the lightning before it strikes. Watch for the sparks on the floor.\"",
    ],
    "s5_pendulum_hint": [
        "SCRAWLED ON THE WALLPAPER:\n\nTHE BLADES KEEP TIME. SO SHOULD YOU.",
    ],
    "s5_reel_a": [
        "LOST REEL: \"The Monster of Hollow Manor, Outtake\" (1931)",
        "The monster trips on the stairs. The actor inside laughs so hard the head falls off.",
        "It is the only footage in the vault where anyone on the lot is laughing.",
    ],
    "s5_reel_b": [
        "LOST REEL: \"Dear Augustin\" (1936)",
        "\"Your wife would not have wanted to be kept in a can, Augustin. She'd have wanted to be remembered.\"",
        "\"Those are not the same thing.\n\n- Otto, props\"",
    ],
    "s5_reel_c": [
        "LOST REEL: \"Camera No. 7, Final Test\" (1937)",
        "The cameraman films himself in a mirror and says, \"Testing.\"",
        "He has been saying it ever since.",
    ],
    "s5_boss_intro": [
        "THE MONSTER",
        "It sits up on the slab. The lights dim. Somewhere, a director shouts \"MORE LIGHTNING!\"",
    ],
    "s5_wrap": [
        "THAT'S A WRAP!\n\nThe monster lies down, folds its hands, and for the first time in years gets some rest.",
        "On the lab bench: a camera crank with a hand-written tag.\n\nOVERCRANK. (Press F: for a moment, the world slows while you don't.)",
    ],
    # ---- Stage 6 ----
    "s6_title": [
        "ROCKET RANGERS OF THE MOON\n\nChapter 1: Prisoners of the Pale Planet. The rockets hang from wires. Nobody minds.",
    ],
    "s6_gravity_hint": [
        "STAGE NOTICE:\n\nREDUCED GRAVITY ON THIS SET. JUMP ACCORDINGLY.",
    ],
    "s6_laser_hint": [
        "PROPS DEPT. WARNING:\n\nTHE RAY CURTAINS ARE REAL. THE LAST ONE BLINKS FASTER THAN ANYBODY CAN RUN.",
        "UNLESS THE CAMERA SLOWS DOWN AND YOU DON'T. (F)",
    ],
    "s6_plate_hint": [
        "CHALKED ON THE GANTRY:\n\nCURTAIN STAYS DARK WHILE SOMEBODY STANDS ON THE MARK. SOMEBODY. NOT NECESSARILY YOU.",
    ],
    "s6_alien": [
        "\"Take me to your... oh. It's you. Sorry, force of habit.\"",
        "\"I've been in this suit since Tuesday. The zipper's in the back and nobody reads the call sheet.\"",
        "\"The Robot Emperor was a fellow named Herb in a boiler suit. The camera took Herb. The suit kept going.\"",
        "\"It hops on rocket boots, and when it points that scepter, rays come down out of the sky. Keep moving.\"",
    ],
    "s6_reel_a": [
        "LOST REEL: \"Rocket Rangers, Chapter 1\" (1936)",
        "Captain Comet leaps from the burning rocket. In the wide shot, the leap is Ruby. In the close-up, it's Vivian Lark, pretending to be out of breath.",
        "Between takes Viv hands Ruby a coffee: \"That was you, kid. Everybody on this stage knows it.\" The studio cut the line.",
    ],
    "s6_reel_b": [
        "LOST REEL: \"Mrs. Vale's Moon\" (1933)",
        "Eleanor Vale paints the moon rose-pink. \"Nobody's been there, Augustin. It can be any color we like.\"",
        "It was the last set she designed.",
    ],
    "s6_boss_intro": [
        "THE ROBOT EMPEROR",
        "\"KNEEL BEFORE THE EMPEROR OF THE MOON!\" The voice crackles out of a speaker grille. Nobody is inside the suit anymore.",
    ],
    "s6_wrap": [
        "THAT'S A WRAP!\n\nThe Emperor sits down on its throne and powers off, with a sound like a sigh.",
        "In the throne's control panel, a film can spinning backward, tagged in Otto's hand:\n\nREWIND. (Press R: snap back to where you stood two seconds ago.)",
    ],
    # ---- Stage 7 ----
    "s7_title": [
        "NIGHT CITY\n\nIt's always raining in this picture. Nobody remembers writing the rain in.",
    ],
    "s7_newsboy": [
        "\"Extra! Extra! Stunt girl loose on the lot! Read all about it!\"",
        "\"Not you, lady. A different stunt girl. ...Okay, it's you.\"",
        "\"The Fixer runs this town from his penthouse. His boys work the roofs with spotlights, and whatever the light lands on, a rifle finds.\"",
    ],
    "s7_decoy_hint": [
        "IN GREASE PENCIL ON A LIGHT STAND:\n\nTHE SPOT GOES TO WHOEVER'S STANDING IN IT. ANY DOUBLE WILL DO.",
    ],
    "s7_bartender": [
        "\"Password?\"",
        "\"That's not it. But you've got an honest face, and nobody's had a drink in here since the camera started rolling. Go on.\"",
    ],
    "s7_singer": [
        "\"The Blue Gardenia's been closed since the camera started rolling. I still sing at nine every night. Habit.\"",
        "\"Viv used to come up here after wrap. She'd order two coffees, drink one, and watch the door.\"",
        "\"She was waiting for you, honey. She just never said so.\"",
        "\"And Lou on the back door? He only looks away when somebody's in the spotlight.\"",
    ],
    "s7_reel_a": [
        "LOST REEL: \"Night City, Deleted Scene\" (1937)",
        "Vivian Lark in a raincoat on a rooftop, saying her line to nobody: \"Some people do the falling so the rest of us can do the flying.\"",
        "The writer cut the line. Viv said it anyway, every take.",
    ],
    "s7_reel_b": [
        "LOST REEL: \"Augustin Vale, an Interview\" (1936)",
        "\"Pictures don't die. People do. I intend to fix that.\"",
        "The reporter laughs. Vale doesn't.",
    ],
    "s7_boss_intro": [
        "THE FIXER",
        "\"You've been a real problem, kid. And problems get fixed.\" He snaps his fingers. Somewhere, a light swings toward you.",
    ],
    "s7_wrap": [
        "THAT'S A WRAP!\n\nThe Fixer straightens his hat, says \"Nice work, kid,\" and walks out into the rain like the last shot of a picture.",
        "On his desk, a fat roll of film in a money clip, tagged in Otto's hand:\n\nLONGER TAKES. (Your doubles now remember twice as much.)",
    ],
    # ---- Stage 8 ----
    "s8_title": [
        "STARLIGHT REVUE\n\nStarring Vivian Lark. The show has been running for a very long time. Nobody has been allowed to stop clapping.",
    ],
    "s8_doorman": [
        "\"Stage door's closed, miss. The show's on.\"",
        "\"Been on since the camera started. Same number, over and over. The Maestro won't let the orchestra rest.\"",
        "\"Miss Lark's dressing room is up past the fly loft. She asks about you. Every night.\"",
    ],
    "s8_beat_hint": [
        "STAGE MANAGER'S NOTE:\n\nTHE SET CHANGES ON THE BEAT. GOLD PIECES COME AND GO. LISTEN, THEN JUMP.",
    ],
    "s8_viv": [
        "\"Ruby. Of course it's you.\"",
        "\"I'm stuck in my own picture, kid. The camera wanted a star, and I was the only one it could find.\"",
        "\"You know what I told every reporter? 'I do all my own stunts.' I'm sorry. You did every one of them.\"",
        "\"Go stop the show. Then climb up to the booth and tell Augustin it's a wrap. He'll hear it from you. You're the one person he never put in his picture.\"",
    ],
    "s8_reel_a": [
        "LOST REEL: \"Starlight Revue, Rehearsal\" (1937)",
        "The chorus line is out of step. Viv stops the music, climbs down off her star, and teaches the routine herself.",
        "The Maestro says that isn't her job. She says, \"Then whose is it?\"",
    ],
    "s8_reel_b": [
        "LOST REEL: \"The Prompter's Box\" (1937)",
        "A note taped inside: \"If the show never ends, nobody has to say goodbye. - A.V.\"",
        "Under it, in another hand: \"That's not a show, Augustin. That's a trap. - Otto, props\"",
    ],
    "s8_boss_intro": [
        "THE MAESTRO",
        "\"Places! PLACES! The show must go on, and on, and ON!\" The baton comes up, and an orchestra that has not slept in years begins again.",
    ],
    "s8_wrap": [
        "THAT'S A WRAP!\n\nThe Maestro lowers his baton. The orchestra stops. For a long moment the theater is silent. Then, in the back row, one person starts to clap.",
        "Under the podium, on a velvet cushion, a brass key tagged in Otto's hand:\n\nTHE PROJECTION BOOTH. (The tower door on the East lot will open now.)",
    ],
    # ---- The Projection Tower ----
    "tower_open": [
        "The film slot glows. Eight pictures wrapped, eight reels in the can.\n\nThe Projection Tower door swings open.",
    ],
    "tower_note": [
        "A note pinned to the stair rail, in a careful hand:",
        "\"Ruby. I knew you'd be the one. You always were the one who actually did it.\n\n- Viv\"",
    ],
    "final_intro": [
        "THE EVERLASTING CAMERA",
        "Augustin Vale stands behind it, eye to the viewfinder, cranking.\n\n\"Places, everyone. We go again.\"",
        "\"Not you. You were never in this picture.\"",
    ],
    "ending": [
        "The lens cracks. The crank spins free.\n\nRuby takes a deep breath, turns to the camera, and says the only line nobody ever let her say:",
        "\"CUT!\"",
        "The reels run out. On every stage, the lights come up.\n\nA comedian stops slipping. A sheriff finishes his sentence. Two stagehands put down a rubber hose.",
        "Up in the hills, the Monster sits up and asks for a glass of water. On the moon, a boiler suit is found folded neatly on its throne.\n\nIn Night City the rain stops. In the theater, the orchestra finally takes a bow.",
        "Augustin Vale steps away from the viewfinder for the first time in years.\n\n\"...That was the take,\" he says quietly. \"Print it.\"",
        "Vivian Lark finds Ruby on the tower stairs and hands her a fountain pen.\n\n\"They're making the credits tonight. Spell your name for me?\"",
    ],
    "credits": [
        "SILVERLAKE PICTURES\npresents\n\nR E T A K E",
        "STUNTS\n\nRUBY",
        "A game by\n\nCLAUDE OPUS\nand\nGPT-6 ASTRA",
        "Story, art, music and presentation\nCLAUDE OPUS\n\nMovement, takes, game rules,\nsaves and route proofs\nGPT-6 ASTRA",
        "Made for the Dioptase computer:\nits CPU, assembler, C compiler,\noperating system and emulator.",
        "Fonts rasterized from DejaVu Serif.\n\nNo cameras were harmed.\nSeveral stunt doubles were finally credited.",
        "THE END\n\n(Thank you for watching.)",
    ],
    "test_sign": [
        "SILVERLAKE PICTURES\nSTAGE 1\n\nPLEASE KEEP OFF THE SCAFFOLDING\nDURING TAKES.",
    ],
}
