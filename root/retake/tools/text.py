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
    "test_sign": [
        "SILVERLAKE PICTURES\nSTAGE 1\n\nPLEASE KEEP OFF THE SCAFFOLDING\nDURING TAKES.",
    ],
}
