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
        "STAGE MANAGER'S NOTE:\n\nEvery actor must hit their MARK.\nThe set won't move until somebody's standing on it.",
        "(Press C to start recording a take. Stand on the mark, then press C again. Your take will keep standing there, and you can go on without it.)",
    ],
    "s1_double_hint": [
        "STUNT COORDINATOR'S NOTE:\n\nCan't reach it? Two stunt doubles are better than one.",
        "(Record a take standing still, then jump onto your own head.)",
    ],
    "s1_wall_hint": [
        "RIGGING CREW:\n\nWall kicks only. Mind the sandbags.",
        "(Jump while sliding down a wall to kick off it.)",
    ],
    "s1_clap_hint": [
        "PROPS:\n\nOne clapperboard, slightly haunted.",
        "(Press X to clap. Hold up or down to clap upward or below you. Clap downward on an enemy to bounce off it.)",
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
    "test_sign": [
        "SILVERLAKE PICTURES\nSTAGE 1\n\nPLEASE KEEP OFF THE SCAFFOLDING\nDURING TAKES.",
    ],
}
