#include "AppHdr.h"

#include "godconduct.h"

#include "fight.h"
#include "godwrath.h"
#include "libutil.h"
#include "message.h"
#include "monster.h"
#include "mon-util.h"
#include "religion.h"
#include "state.h"

/////////////////////////////////////////////////////////////////////
// god_conduct_trigger

god_conduct_trigger::god_conduct_trigger(
    conduct_type c, int pg, bool kn, const monster* vict)
  : conduct(c), pgain(pg), known(kn), enabled(true), victim(nullptr)
{
    if (vict)
    {
        victim.reset(new monster);
        *(victim.get()) = *vict;
    }
}

void god_conduct_trigger::set(conduct_type c, int pg, bool kn,
                              const monster* vict)
{
    conduct = c;
    pgain = pg;
    known = kn;
    victim.reset(NULL);
    if (vict)
    {
        victim.reset(new monster);
        *victim.get() = *vict;
    }
}

god_conduct_trigger::~god_conduct_trigger()
{
    if (enabled && conduct != NUM_CONDUCTS)
        did_god_conduct(conduct, pgain, known, victim.get());
}

#ifdef DEBUG_DIAGNOSTICS
static const char *conducts[] =
{
    "",
    "Necromancy", "Holy", "Unholy", "Attack Holy", "Attack Neutral",
    "Attack Friend", "Friend Died", "Unchivalric Attack",
    "Poison", "Kill Living", "Kill Undead",
    "Kill Demon", "Kill Natural Unholy", "Kill Natural Evil",
    "Kill Unclean", "Kill Chaotic", "Kill Wizard", "Kill Priest",
    "Kill Holy", "Kill Fast", "Undead Slave Kill Living",
    "Servant Kill Living", "Undead Slave Kill Undead",
    "Servant Kill Undead", "Undead Slave Kill Demon",
    "Servant Kill Demon", "Servant Kill Natural Unholy",
    "Servant Kill Natural Evil", "Undead Slave Kill Holy",
    "Servant Kill Holy", "Banishment", "Spell Memorise", "Spell Cast",
    "Spell Practise",
    "Drink Blood", "Cannibalism","Eat Souled Being",
    "Deliberate Mutation", "Cause Glowing", "Use Unclean",
    "Use Chaos", "Desecrate Orcish Remains", "Destroy Orcish Idol",
    "Kill Slime", "Kill Plant", "Servant Kill Plant",
    "Was Hasty", "Corpse Violation",
    "Souled Friend Died", "Servant Kill Unclean",
    "Servant Kill Chaotic", "Attack In Sanctuary",
    "Kill Artificial", "Undead Slave Kill Artificial",
    "Servant Kill Artificial", "Destroy Spellbook",
    "Exploration", "Desecrate Holy Remains", "Seen Monster",
    "Fire", "Kill Fiery", "Sacrificed Love"
};
#endif

/**
 * Change piety & add penance in response to a conduct.
 *
 * @param piety_change      The change in piety (+ or -) the conduct caused.
 * @param piety_denom       ???
 * @param penance           Penance caused by the conduct.
 * @param thing_done        The conduct in question. Used for debug info.
 */
static void _handle_piety_penance(int piety_change, int piety_denom,
                                  int penance, conduct_type thing_done)
{
#ifdef DEBUG_DIAGNOSTICS
    const int old_piety = you.piety;
#else
    UNUSED(thing_done);
#endif

    if (piety_change > 0)
        gain_piety(piety_change, piety_denom);
    else
        dock_piety(div_rand_round(-piety_change, piety_denom), penance);

#ifdef DEBUG_DIAGNOSTICS
    // don't announce exploration piety unless you actually got a boost
    if ((piety_change || penance)
        && thing_done != DID_EXPLORATION || old_piety != you.piety)
    {

        COMPILE_CHECK(ARRAYSZ(conducts) == NUM_CONDUCTS);
        dprf("conduct: %s; piety: %d (%+d/%d); penance: %d (%+d)",
             conducts[thing_done],
             you.piety, piety_change, piety_denom,
             you.penance[you.religion], penance);

    }
#endif

}

/**
 * Whether good gods that you folow are offended by you attacking a specific
 * holy monster.
 *
 * @param victim    The holy in question. (May be NULL.)
 * @return          Whether DID_ATTACK_HOLY applies.
 */
static bool _attacking_holy_matters(const monster* victim)
{
    // XXX: what does this do?
    return !victim
            || testbits(victim->flags, MF_NO_REWARD)
            || testbits(victim->flags, MF_WAS_NEUTRAL);
}




/// A definition of the way in which a god dislikes a conduct being taken.
struct dislike_response
{
    /// Loss in piety for triggering this conduct; multiplied by 'level'.
    int piety_factor;
    /// Penance for triggering this conduct; multiplied by 'level'.
    int penance_factor;
    /// Something your god says when you accidentally triggered the conduct.
    /// Implies that unknowingly triggering the conduct is forgiven.
    const char *forgiveness_message;
    /// Something your god says when you trigger this conduct. May be NULL.
    const char *message;
    /// A function that checks the victim of the conduct to see if the conduct
    /// should actually, really apply to it. If NULL, all victims are valid.
    bool (*valid_victim)(const monster* victim);
};



/// Good gods' reaction to drinking blood.
static const dislike_response GOOD_BLOOD_RESPONSE = {
    2, 1, " forgives your inadvertent blood-drinking, just this once."
};

/// Good gods', and Beogh's, response to cannibalism.
static const dislike_response RUDE_CANNIBALISM_RESPONSE = {
    5, 3, NULL, " expects more respect for your departed relatives."
};

/// Zin and Ely's responses to desecrating holy remains.
static const dislike_response GOOD_DESECRATE_HOLY_RESPONSE = {
    1, 1, NULL, " expects more respect for holy creatures!"
};

/// Zin and Ely's responses to unholy actions & necromancy.
static const dislike_response GOOD_UNHOLY_RESPONSE = {
    1, 1, " forgives your inadvertent unholy act, just this once."
};

/// Zin and Ely's responses to the player attacking holy creatures.
static const dislike_response GOOD_ATTACK_HOLY_RESPONSE = {
    1, 1, NULL, NULL, _attacking_holy_matters
};

/// Good gods' response to the player killing holy creatures.
static const dislike_response GOOD_KILL_HOLY_RESPONSE = {
    3, 3, NULL, NULL, _attacking_holy_matters
};

/// TSO's response to the player stabbing or poisoning monsters.
static const dislike_response TSO_UNCHIVALRIC_RESPONSE = {
    1, 2, " forgives your inadvertent dishonourable attack, just"
              " this once.", NULL, [] (const monster* victim) {
        return !victim || !tso_unchivalric_attack_safe_monster(victim);
    }
};

/// TSO and Ely's response to the player attacking neutral monsters.
static const dislike_response GOOD_ATTACK_NEUTRAL_RESPONSE = {
    1, 1, " forgives your inadvertent attack on a neutral, just this once."
};

/// Various gods' response to attacking a pal.
static const dislike_response ATTACK_FRIEND_RESPONSE = {
    1, 3, " forgives your inadvertent attack on an ally, just this once.",
    NULL, [] (const monster* victim) {
        dprf("hates friend : %d", god_hates_attacking_friend(you.religion, victim));
        return god_hates_attacking_friend(you.religion, victim);
    }
};

/// Ely response to a friend dying.
static const dislike_response ELY_FRIEND_DEATH_RESPONSE = {
    1, 0, NULL, NULL, [] (const monster* victim) {
        // For everyone but Fedhas, plants are items not creatures,
        // and animated items are, well, items as well.
        return victim && !mons_is_object(victim->type)
                      && victim->holiness() != MH_PLANT
        // Converted allies (marked as TSOites) can be martyrs.
                      && victim->god == GOD_SHINING_ONE;
    }
};

/// Fedhas's response to a friend(ly plant) dying.
static const dislike_response FEDHAS_FRIEND_DEATH_RESPONSE = {
    1, 0, NULL, NULL, [] (const monster* victim) {
        // ballistomycetes are penalized separately.
        return victim && fedhas_protects(victim)
        && victim->mons_species() != MONS_BALLISTOMYCETE;
    }
};

typedef map<conduct_type, dislike_response> peeve_map;

/// a per-god map of conducts to that god's angry reaction to those conducts.
static peeve_map divine_peeves[] =
{
    // GOD_NO_GOD
    peeve_map(),
    // GOD_ZIN,
    {
        { DID_DRINK_BLOOD, GOOD_BLOOD_RESPONSE },
        { DID_CANNIBALISM, RUDE_CANNIBALISM_RESPONSE },
        { DID_ATTACK_HOLY, GOOD_ATTACK_HOLY_RESPONSE },
        { DID_KILL_HOLY, GOOD_KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_SERVANT, GOOD_KILL_HOLY_RESPONSE },
        { DID_DESECRATE_HOLY_REMAINS, GOOD_DESECRATE_HOLY_RESPONSE },
        { DID_NECROMANCY, GOOD_UNHOLY_RESPONSE },
        { DID_UNHOLY, GOOD_UNHOLY_RESPONSE },
        { DID_ATTACK_FRIEND, ATTACK_FRIEND_RESPONSE },
    },
    // GOD_SHINING_ONE,
    {
        { DID_DRINK_BLOOD, GOOD_BLOOD_RESPONSE },
        { DID_CANNIBALISM, RUDE_CANNIBALISM_RESPONSE },
        { DID_ATTACK_HOLY, {
            1, 2, NULL, NULL, _attacking_holy_matters
        } },
        { DID_KILL_HOLY, GOOD_KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_SERVANT, GOOD_KILL_HOLY_RESPONSE },
        { DID_DESECRATE_HOLY_REMAINS, {
            1, 2, NULL, " expects more respect for holy creatures!"
        } },
        { DID_NECROMANCY, {
            1, 2, " forgives your inadvertent unholy act, just this once."
        } },
        { DID_UNHOLY, {
            1, 2, " forgives your inadvertent unholy act, just this once."
        } },
        { DID_UNCHIVALRIC_ATTACK, TSO_UNCHIVALRIC_RESPONSE },
        { DID_POISON, TSO_UNCHIVALRIC_RESPONSE },
        { DID_ATTACK_NEUTRAL, GOOD_ATTACK_NEUTRAL_RESPONSE },
        { DID_ATTACK_FRIEND, ATTACK_FRIEND_RESPONSE },
    },
    // GOD_KIKUBAAQUDGHA,
    peeve_map(),
    // GOD_YREDELEMNUL,
    {
        { DID_HOLY, {
            1, 2, " forgives your inadvertent holy act, just this once."
        } },
    },
    // GOD_XOM,
    peeve_map(),
    // GOD_VEHUMET,
    peeve_map(),
    // GOD_OKAWARU,
    {
        { DID_ATTACK_FRIEND, ATTACK_FRIEND_RESPONSE },
    },
    // GOD_MAKHLEB,
    peeve_map(),
    // GOD_SIF_MUNA,
    peeve_map(),
    // GOD_TROG,
    {
        { DID_SPELL_MEMORISE, {
            10, 10
        } },
        { DID_SPELL_CASTING, {
            1, 5,
        } },
        { DID_SPELL_PRACTISE, {
            1, 0, NULL, " doesn't appreciate your training magic!"
        } },
    },
    // GOD_NEMELEX_XOBEH,
    peeve_map(),
    // GOD_ELYVILON,
    {
        { DID_DRINK_BLOOD, GOOD_BLOOD_RESPONSE },
        { DID_CANNIBALISM, RUDE_CANNIBALISM_RESPONSE },
        { DID_ATTACK_HOLY, GOOD_ATTACK_HOLY_RESPONSE },
        { DID_KILL_HOLY, GOOD_KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_SERVANT, GOOD_KILL_HOLY_RESPONSE },
        { DID_DESECRATE_HOLY_REMAINS, GOOD_DESECRATE_HOLY_RESPONSE },
        { DID_NECROMANCY, GOOD_UNHOLY_RESPONSE },
        { DID_UNHOLY, GOOD_UNHOLY_RESPONSE },
        { DID_ATTACK_NEUTRAL, GOOD_ATTACK_NEUTRAL_RESPONSE },
        { DID_ATTACK_FRIEND, ATTACK_FRIEND_RESPONSE },
        { DID_FRIEND_DIED, ELY_FRIEND_DEATH_RESPONSE },
        { DID_SOULED_FRIEND_DIED, ELY_FRIEND_DEATH_RESPONSE },
        { DID_KILL_LIVING, {
            1, 2, NULL, " does not appreciate your shedding blood"
                            " when asking for salvation!",
            [] (const monster* _) {
                UNUSED(_);
                // Killing is only disapproved of during prayer.
                return you.duration[DUR_LIFESAVING] != 0;
            }
        } },
    },
    // GOD_LUGONU,
    peeve_map(),
    // GOD_BEOGH,
    {
        { DID_CANNIBALISM, RUDE_CANNIBALISM_RESPONSE },
        { DID_ATTACK_FRIEND, ATTACK_FRIEND_RESPONSE },
    },
    // GOD_JIYVA,
    {
        { DID_KILL_SLIME, {
            1, 2, NULL, NULL, [] (const monster* victim) {
                return victim && !victim->is_shapeshifter();
            }
        } },
        { DID_ATTACK_NEUTRAL, {
            1, 1, NULL, NULL, [] (const monster* victim) {
                return victim
                    && mons_is_slime(victim) && !victim->is_shapeshifter();
            }
        } },
        { DID_ATTACK_FRIEND, ATTACK_FRIEND_RESPONSE },
    },
    // GOD_FEDHAS,
    {
        { DID_CORPSE_VIOLATION, {
            1, 1, " forgives your inadvertent necromancy, just this once."
        } },
        { DID_KILL_PLANT, {
            1, 0
        } },
        { DID_PLANT_KILLED_BY_SERVANT, {
            1, 0
        } },
        { DID_ATTACK_FRIEND, ATTACK_FRIEND_RESPONSE },
        { DID_FRIEND_DIED, FEDHAS_FRIEND_DEATH_RESPONSE },
        { DID_SOULED_FRIEND_DIED, FEDHAS_FRIEND_DEATH_RESPONSE },
    },
    // GOD_CHEIBRIADOS,
    peeve_map(),
    // GOD_ASHENZARI,
    peeve_map(),
    // GOD_DITHMENOS,
    peeve_map(),
    // GOD_GOZAG,
    peeve_map(),
    // GOD_QAZLAL,
    peeve_map(),
    // GOD_RU,
    peeve_map(),
};



/// A definition of the way in which a god likes a conduct being taken.
struct like_response
{
    /// Gain in piety for triggering this conduct; added to calculated denom.
    int piety_bonus;
    /// Divider for piety gained by this conduct; added to 'level'.
    int piety_denom_bonus;
    /// Degree to which your XL modifies piety gained.
    int xl_denom;
    /// Something your god says when you trigger this conduct. May be NULL.
    const char *message;
    /// A function that checks the victim of the conduct to see if the conduct
    /// should actually, really apply to it. If NULL, all victims are valid.
    bool (*valid_victim)(const monster* victim);
};

/**
 * Will your god give you piety for killing the given monster, assuming that
 * its death triggers a conduct that the god sometimes likes?
 */
static bool _god_likes_killing(const monster* victim)
{
    if (you_worship(GOD_DITHMENOS)
        && mons_class_flag(victim->type, M_SHADOW))
    {
        return false;
    }

    return !god_hates_attacking_friend(you.religion, victim);
}

/// Response for gods that like killing the living.
static const like_response KILL_LIVING_RESPONSE = {
    -6, 18, 2, " accepts your kill.", _god_likes_killing
};

/// Response for non-good gods that like killing (?) undead.
static const like_response KILL_UNDEAD_RESPONSE = {
    -5, 18, 2, " accepts your kill.", _god_likes_killing
};

/// Response for non-good gods that like killing (?) demons.
static const like_response KILL_DEMON_RESPONSE = {
    -4, 18, 2, " accepts your kill.", _god_likes_killing
};

/// Response for TSO/Zin when you kill (most) things they hate.
static const like_response GOOD_KILL_RESPONSE = {
    -4, 18, 0, " accepts your kill.", _god_likes_killing
};

/// Response for non-good gods that like killing (?) holies. also, yred
/// uses this for killing artificials.
static const like_response KILL_HOLY_RESPONSE = {
    -3, 18, 0, " accepts your kill.", _god_likes_killing
};
// Note that holy deaths are special - they're always noticed...
// If you or any friendly kills one, you'll get the credit/blame.

/// Response for gods that like your undead slaves killing holies.
/// Also, Yred uses this for killing artificials.
static const like_response UNDEAD_KILL_HOLY_RESPONSE = {
    -3, 18, 0, " accepts your slave's kill.", _god_likes_killing
};

/// Response for gods that like your servants killing holies.
static const like_response SERVANT_KILL_HOLY_RESPONSE = {
    -3, 18, 0, " accepts your collateral kill.", _god_likes_killing
};

/// Response for gods that like your undead slaves killing non-holy things.
static const like_response UNDEAD_KILL_RESPONSE = {
    -6, 10, 3, " accepts your slave's kill."
};

/// Response for gods that like your servants killing non-holy things.
static const like_response SERVANT_KILL_RESPONSE = {
    -6, 10, 3, " accepts your collateral kill."
};

/// Response for TSO/Zin when your servants kill things they hate.
static const like_response GOOD_COLLATERAL_RESPONSE = {
    -6, 10, 0, " accepts your collateral kill."
};


typedef map<conduct_type, like_response> like_map;

/// a per-god map of conducts to piety rewards given by that god.
static like_map divine_likes[] =
{
    // GOD_NO_GOD
    like_map(),
    // GOD_ZIN,
    {
        { DID_KILL_UNCLEAN, GOOD_KILL_RESPONSE },
        { DID_KILL_CHAOTIC, GOOD_KILL_RESPONSE },
        { DID_UNCLEAN_KILLED_BY_SERVANT, GOOD_COLLATERAL_RESPONSE },
        { DID_CHAOTIC_KILLED_BY_SERVANT, GOOD_COLLATERAL_RESPONSE },
    },
    // GOD_SHINING_ONE,
    {
        { DID_KILL_UNDEAD, {
            -5, 18, 0, " accepts your kill.", _god_likes_killing
        } },
        { DID_KILL_DEMON, GOOD_KILL_RESPONSE },
        { DID_KILL_NATURAL_UNHOLY, GOOD_KILL_RESPONSE },
        { DID_KILL_NATURAL_EVIL, GOOD_KILL_RESPONSE },
        { DID_UNDEAD_KILLED_BY_SERVANT, GOOD_COLLATERAL_RESPONSE },
        { DID_DEMON_KILLED_BY_SERVANT, GOOD_COLLATERAL_RESPONSE },
        { DID_NATURAL_UNHOLY_KILLED_BY_SERVANT, GOOD_COLLATERAL_RESPONSE },
        { DID_NATURAL_EVIL_KILLED_BY_SERVANT, GOOD_COLLATERAL_RESPONSE },
    },
    // GOD_KIKUBAAQUDGHA,
    {
        { DID_KILL_LIVING, KILL_LIVING_RESPONSE },
        { DID_KILL_DEMON, KILL_DEMON_RESPONSE },
        { DID_KILL_HOLY, KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_HOLY_RESPONSE },
        { DID_LIVING_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
        { DID_DEMON_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
    },
    // GOD_YREDELEMNUL,
    {
        { DID_KILL_LIVING, KILL_LIVING_RESPONSE },
        { DID_KILL_HOLY, KILL_HOLY_RESPONSE },
        { DID_KILL_ARTIFICIAL, KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_HOLY_RESPONSE },
        { DID_ARTIFICIAL_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_HOLY_RESPONSE },
        { DID_LIVING_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
    },
    // GOD_XOM,
    like_map(),
    // GOD_VEHUMET,
    {
        { DID_KILL_LIVING, KILL_LIVING_RESPONSE },
        { DID_KILL_UNDEAD, KILL_UNDEAD_RESPONSE },
        { DID_KILL_DEMON, KILL_DEMON_RESPONSE },
        { DID_KILL_HOLY, KILL_HOLY_RESPONSE },
    },
    // GOD_OKAWARU,
    like_map(),
    // GOD_MAKHLEB,
    {
        { DID_KILL_LIVING, KILL_LIVING_RESPONSE },
        { DID_KILL_UNDEAD, KILL_UNDEAD_RESPONSE },
        { DID_KILL_DEMON, KILL_DEMON_RESPONSE },
        { DID_KILL_HOLY, KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_SERVANT, SERVANT_KILL_HOLY_RESPONSE },
        { DID_LIVING_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
        { DID_LIVING_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_UNDEAD_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
        { DID_UNDEAD_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_DEMON_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
        { DID_DEMON_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
    },
    // GOD_SIF_MUNA,
    like_map(),
    // GOD_TROG,
    {
        { DID_KILL_LIVING, KILL_LIVING_RESPONSE },
        { DID_KILL_DEMON, KILL_DEMON_RESPONSE },
        { DID_KILL_HOLY, KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_SERVANT, SERVANT_KILL_HOLY_RESPONSE },
        { DID_LIVING_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_DEMON_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_KILL_WIZARD, {
            -6, 10, 0, " appreciates your killing of a magic user.",
            _god_likes_killing
        } },
    },
    // GOD_NEMELEX_XOBEH,
    like_map(),
    // GOD_ELYVILON,
    like_map(),
    // GOD_LUGONU,
    {
        { DID_KILL_LIVING, KILL_LIVING_RESPONSE },
        { DID_KILL_UNDEAD, KILL_UNDEAD_RESPONSE },
        { DID_KILL_DEMON, KILL_DEMON_RESPONSE },
        { DID_KILL_HOLY, KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_SERVANT, SERVANT_KILL_HOLY_RESPONSE },
        { DID_LIVING_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
        { DID_LIVING_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_UNDEAD_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
        { DID_UNDEAD_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_DEMON_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
        { DID_DEMON_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_BANISH, {
            -6, 18, 2, " claims a new guest."
        } },
    },
    // GOD_BEOGH,
    {
        { DID_KILL_LIVING, KILL_LIVING_RESPONSE },
        { DID_KILL_UNDEAD, KILL_UNDEAD_RESPONSE },
        { DID_KILL_DEMON, KILL_DEMON_RESPONSE },
        { DID_KILL_HOLY, KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_SERVANT, SERVANT_KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_HOLY_RESPONSE },
        { DID_LIVING_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
        { DID_LIVING_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_UNDEAD_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
        { DID_UNDEAD_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_DEMON_KILLED_BY_UNDEAD_SLAVE, UNDEAD_KILL_RESPONSE },
        { DID_DEMON_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_KILL_PRIEST, {
            -6, 10, 0, " appreciates your killing of a heretic priest.",
            _god_likes_killing
        } },
    },
    // GOD_JIYVA,
    like_map(),
    // GOD_FEDHAS,
    like_map(),
    // GOD_CHEIBRIADOS,
    {
        { DID_KILL_FAST, {
            -6, 18, 2, NULL, _god_likes_killing
        } }
    },
    // GOD_ASHENZARI,
    like_map(),
    // GOD_DITHMENOS,
    {
        { DID_KILL_LIVING, KILL_LIVING_RESPONSE },
        { DID_KILL_UNDEAD, KILL_UNDEAD_RESPONSE },
        { DID_KILL_DEMON, KILL_DEMON_RESPONSE },
        { DID_KILL_HOLY, KILL_HOLY_RESPONSE },
        { DID_KILL_FIERY, {
            -6, 10, 0, " appreciates your extinguishing a source of fire.",
            _god_likes_killing
        } },
    },
    // GOD_GOZAG,
    like_map(),
    // GOD_QAZLAL,
    {
        { DID_KILL_LIVING, KILL_LIVING_RESPONSE },
        { DID_KILL_UNDEAD, KILL_UNDEAD_RESPONSE },
        { DID_KILL_DEMON, KILL_DEMON_RESPONSE },
        { DID_KILL_HOLY, KILL_HOLY_RESPONSE },
        { DID_HOLY_KILLED_BY_SERVANT, SERVANT_KILL_HOLY_RESPONSE },
        { DID_LIVING_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_UNDEAD_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
        { DID_DEMON_KILLED_BY_SERVANT, SERVANT_KILL_RESPONSE },
    },
    // GOD_RU,
    like_map(),
};

/**
 * What piety multiplier does chei give for killing a given monster?
 *
 * @param victim        The monster in question.
 * @return              A multiplier to piety gained from killing the monster.
 */
static int _chei_speed_piety_mult(const monster* victim)
{
    const int speed_delta =
        cheibriados_monster_player_speed_delta(victim);
    dprf("Chei DID_KILL_FAST: %s speed delta: %d",
         victim->name(DESC_PLAIN, true).c_str(),
         speed_delta);

    if (speed_delta > 0 && x_chance_in_y(speed_delta, 12))
    {
        simple_god_message(" thoroughly appreciates the change of pace.");
        return 2;
    }

    simple_god_message(" appreciates the change of pace.");
    return 1;
}

/**
 * What multiplier to 'base' piety gains do you get for performing a given
 * conduct, over that expressed in the data tables?
 *
 * Only affects gains.
 *
 * Currently for weird yred/chei special cases. Possibly should be pulled out
 * into a function in the table.
 *
 * @param thing_done    The conduct.
 * @param victim        The victim of the conduct. Poor thing...
 * @return              A multiplier to be applied to piety gains.
 */
static int _piety_mult(conduct_type thing_done, const monster* victim)
{
    switch (thing_done)
    {
        case DID_KILL_FAST:
            // assumes only chei offers this conduct
            return _chei_speed_piety_mult(victim);
        case DID_KILL_HOLY:
            if (!you_worship(GOD_YREDELEMNUL))
                return 1;

            simple_god_message(" appreciates your killing of a holy being.");
            return 2;
        default:
            return 1;
    }
}



static void _handle_your_gods_response(conduct_type thing_done, int level,
                                       bool known, const monster* victim)
{
    // Lucy gives no piety in Abyss. :(
    // XXX: make this not a hack...? (or remove it?)
    if (you_worship(GOD_LUGONU) && player_in_branch(BRANCH_ABYSS))
        return;

    // handle new-style conduct responses
    // TODO: move everything into here

    // check for dislikes
    COMPILE_CHECK(ARRAYSZ(divine_peeves) == NUM_GODS);
    if (divine_peeves[you.religion].count(thing_done))
    {
        dprf("checking peeve data for %s", conducts[thing_done]);
        const dislike_response peeve = divine_peeves[you.religion][thing_done];

        // if the conduct filters on affected monsters, & the relevant monster
        // isn't valid, don't trigger the conduct's consequences.
        if (peeve.valid_victim && !peeve.valid_victim(victim))
        {
            dprf("invalid victim for %s", conducts[thing_done]);
            return;
        }

        god_acting gdact;

        // If the player didn't have a way to know they were going to trigger
        // the conduct, and the god cares, print a message & bail.
        if (!known && peeve.forgiveness_message)
        {
            dprf("conduct forgiven");
            simple_god_message(peeve.forgiveness_message);
            return;
        }

        // trigger the actual effects of the conduct.

        // a message, if we have one...
        if (peeve.message)
            simple_god_message(peeve.message);

        dprf("applying penance beam");
        // ...and piety/penance.
        _handle_piety_penance(-peeve.piety_factor * level, 1,
                              peeve.penance_factor * level,
                              thing_done);

        return;
    }

    //check for likes
    COMPILE_CHECK(ARRAYSZ(divine_likes) == NUM_GODS);
    if (divine_likes[you.religion].count(thing_done))
    {
        dprf("checking like data for %s", conducts[thing_done]);
        const like_response like = divine_likes[you.religion][thing_done];

        // if the conduct filters on affected monsters, & the relevant monster
        // isn't valid, don't trigger the conduct's consequences.
        if (like.valid_victim && !like.valid_victim(victim))
        {
            dprf("invalid victim for %s", conducts[thing_done]);
            return;
        }

        god_acting gdact;

        if (like.message)
            simple_god_message(like.message);

        // this is all very strange, but replicates legacy behavior.
        int denom = like.piety_denom_bonus + level;
        if (like.xl_denom)
            denom -= you.get_experience_level() / like.xl_denom;

        const int gain_mult = _piety_mult(thing_done, victim);
        const int gain = (denom + like.piety_bonus) * gain_mult;

        _handle_piety_penance(max(0, gain), max(1, denom), 0, thing_done);

        return;
    }

    dprf("no data for %s", conducts[thing_done]);

    if (you_worship(GOD_NO_GOD) || you_worship(GOD_XOM))
        return;

    int piety_change = 0;
    int piety_denom = 1;
    int penance = 0;

    god_acting gdact;

    switch (thing_done)
    {
        case DID_DRINK_BLOOD:
        case DID_CANNIBALISM:
        case DID_CORPSE_VIOLATION:
        case DID_DESECRATE_HOLY_REMAINS:
        case DID_NECROMANCY:
        case DID_UNHOLY:
        case DID_ATTACK_HOLY:
        case DID_HOLY:
        case DID_UNCHIVALRIC_ATTACK:
        case DID_POISON:
        case DID_KILL_SLIME:
        case DID_KILL_PLANT:
        case DID_PLANT_KILLED_BY_SERVANT:
        case DID_ATTACK_NEUTRAL:
        case DID_ATTACK_FRIEND:
        case DID_FRIEND_DIED:
        case DID_SOULED_FRIEND_DIED:
        case DID_BANISH:
        case DID_KILL_LIVING:
        case DID_KILL_UNDEAD:
        case DID_KILL_DEMON:
        case DID_KILL_NATURAL_UNHOLY:
        case DID_KILL_NATURAL_EVIL:
        case DID_KILL_UNCLEAN:
        case DID_KILL_CHAOTIC:
        case DID_KILL_PRIEST:
        case DID_KILL_WIZARD:
        case DID_KILL_FAST:
        case DID_KILL_HOLY:
        case DID_HOLY_KILLED_BY_UNDEAD_SLAVE:
        case DID_HOLY_KILLED_BY_SERVANT:
        case DID_LIVING_KILLED_BY_UNDEAD_SLAVE:
        case DID_LIVING_KILLED_BY_SERVANT:
        case DID_UNDEAD_KILLED_BY_UNDEAD_SLAVE:
        case DID_UNDEAD_KILLED_BY_SERVANT:
        case DID_DEMON_KILLED_BY_UNDEAD_SLAVE:
        case DID_DEMON_KILLED_BY_SERVANT:
        case DID_NATURAL_UNHOLY_KILLED_BY_SERVANT:
        case DID_NATURAL_EVIL_KILLED_BY_SERVANT:
        case DID_UNCLEAN_KILLED_BY_SERVANT:
        case DID_CHAOTIC_KILLED_BY_SERVANT:
        case DID_KILL_ARTIFICIAL:
        case DID_ARTIFICIAL_KILLED_BY_UNDEAD_SLAVE:
        case DID_ARTIFICIAL_KILLED_BY_SERVANT:
        case DID_KILL_FIERY:
        case DID_SPELL_MEMORISE:
        case DID_SPELL_CASTING:
            break; // handled in data code



        case DID_DELIBERATE_MUTATING:
        case DID_CAUSE_GLOWING:
            if (!you_worship(GOD_ZIN))
                break;

            if (!known && thing_done != DID_CAUSE_GLOWING)
            {
                simple_god_message(" forgives your inadvertent chaotic "
                                   "act, just this once.");
                break;
            }

            if (thing_done == DID_CAUSE_GLOWING)
            {
                static int last_glowing_lecture = -1;
                if (!level)
                {
                    simple_god_message(" is not enthusiastic about the "
                                       "mutagenic glow surrounding you.");
                }
                else if (last_glowing_lecture != you.num_turns)
                {
                    last_glowing_lecture = you.num_turns;
                    // Increase contamination within yellow glow.
                    simple_god_message(" does not appreciate the extra "
                                       "mutagenic glow surrounding you!");
                }
            }

            piety_change = -level;
            break;

        case DID_DESECRATE_SOULED_BEING:
            if (!you_worship(GOD_ZIN))
                break;

            simple_god_message(" expects more respect for this departed soul.");
            piety_change = -level * 5;
            penance = level * 3;
            break;

        case DID_UNCLEAN:
            if (!you_worship(GOD_ZIN))
                break;

            if (!known)
            {
                simple_god_message(" forgives your inadvertent unclean act,"
                                   " just this once.");
                break;
            }

            piety_change = -level;
            penance      = level;
            break;

        case DID_CHAOS:
            if (you_worship(GOD_ZIN))
            {
                if (!known)
                {
                    simple_god_message(" forgives your inadvertent chaotic "
                                       "act, just this once.");
                    break;
                }
                piety_change = -level;
                penance      = level;
            }
            break;

        case DID_ATTACK_IN_SANCTUARY:
            if (you_worship(GOD_ZIN))
            {
                piety_change = -level;
                penance      = level;
            }
            break;

        case DID_DESECRATE_ORCISH_REMAINS:
            if (you_worship(GOD_BEOGH))
                piety_change = -level;
            break;

        case DID_DESTROY_ORCISH_IDOL:
            if (you_worship(GOD_BEOGH))
            {
                piety_change = -level;
                penance = level * 3;
            }
            break;

        case DID_HASTY:
            if (you_worship(GOD_CHEIBRIADOS))
            {
                if (!known)
                {
                    simple_god_message(" forgives your accidental hurry, just this once.");
                    break;
                }
                simple_god_message(" thinks you should slow down.");
                piety_change = -level;
                if (level > 5)
                    penance = level - 5;
            }
            break;

        case DID_DESTROY_SPELLBOOK:
            if (you_worship(GOD_SIF_MUNA))
            {
                piety_change = -level;
                penance = level * (known ? 2 : 1);
            }
            break;

        case DID_EXPLORATION:
            if (you_worship(GOD_ASHENZARI))
            {
                const int base_gain = 8; // base gain per dungeon level
                // levels: x1, x1.25, x1.5, x1.75, x2
                piety_change = base_gain + base_gain * you.bondage_level / 4;
                piety_denom = level;
            }
            else if (you_worship(GOD_NEMELEX_XOBEH))
            {
                piety_change = 14;
                piety_denom = level;
            }
            else if (you_worship(GOD_RU))
            {
                ASSERT(you.props.exists("ru_progress_to_next_sacrifice"));
                ASSERT(you.props.exists("available_sacrifices"));
                int sacrifice_count =
                you.props["available_sacrifices"].get_vector().size();
                if (sacrifice_count == 0 && one_chance_in(100))
                {
                    int current_progress =
                    you.props["ru_progress_to_next_sacrifice"]
                    .get_int();
                    you.props["ru_progress_to_next_sacrifice"] =
                    current_progress + 1;
                }
            }
            break;

        case DID_SEE_MONSTER:
            if (you_worship(GOD_SHINING_ONE))
            {
                if (victim && (victim->is_evil() || victim->is_unholy()))
                    break;
                piety_denom = level / 2 + 6 - you.experience_level / 4;
                piety_change = piety_denom - 4;
                piety_denom = max(piety_denom, 1);
                piety_change = max(piety_change, 0);
            }
            break;

        case DID_FIRE:
            if (you_worship(GOD_DITHMENOS))
            {
                if (!known)
                {
                    simple_god_message(" forgives your accidental "
                                       "fire-starting, just this once.");
                    break;
                }
                simple_god_message(" does not appreciate your starting fires!");
                piety_change = -level;
                if (level > 5)
                    penance = level - 5;
            }
            break;

        case DID_SPELL_PRACTISE:
            // XXX: migrate this to data somehow
            if (you_worship(GOD_SIF_MUNA))
            {
                piety_change = level;
                piety_denom = 4;
            }

        case DID_SACRIFICE_LOVE:
        case DID_NOTHING:
        case NUM_CONDUCTS:
            break;
    }

    // currently no constructs and plants
    if ((thing_done == DID_KILL_LIVING
         || thing_done == DID_KILL_UNDEAD
         || thing_done == DID_KILL_DEMON
         || thing_done == DID_KILL_HOLY)
        && !god_hates_attacking_friend(you.religion, victim))
    {
        if (you_worship(GOD_OKAWARU))
        {
            piety_change = get_fuzzied_monster_difficulty(victim);
            dprf("fuzzied monster difficulty: %4.2f", piety_change * 0.01);
            piety_denom = 600;
            if (piety_change > 3200)
                simple_god_message(" appreciates your kill.");
            else if (piety_change > 9) // might still be miniscule
                simple_god_message(" accepts your kill.");
        }
        if (you_worship(GOD_DITHMENOS))
        {
            // Full gains at full piety down to 2/3 at 6* piety.
            // (piety_rank starts at 1, not 0.)
            piety_change *= 25 - piety_rank();
            piety_denom *= 24;
        }
    }

    _handle_piety_penance(piety_change, piety_denom, penance, thing_done);
}

// a sad and shrunken function.
static void _handle_other_gods_response(conduct_type thing_done)
{
    if (thing_done == DID_DESTROY_ORCISH_IDOL)
        beogh_idol_revenge();
}


// This function is the merger of done_good() and naughty().
// Returns true if god was interested (good or bad) in conduct.
void did_god_conduct(conduct_type thing_done, int level, bool known,
                     const monster* victim)
{
    ASSERT(!crawl_state.game_is_arena());

    _handle_your_gods_response(thing_done, level, known, victim);
    _handle_other_gods_response(thing_done);
}

// These two arrays deal with the situation where a beam hits a non-fleeing
// monster, the monster starts to flee because of the damage, and then the
// beam bounces and hits the monster again.  If the monster wasn't fleeing
// when the beam started then hits from bounces shouldn't count as
// unchivalric attacks, but if the first hit from the beam *was* unchivalrous
// then all the bounces should count as unchivalrous as well.
//
// Also do the same sort of check for harming a friendly monster,
// since a Beogh worshipper zapping an orc with lightning might cause it to
// become a follower on the first hit, and the second hit would be
// against a friendly orc.
static FixedBitVector<MAX_MONSTERS> _first_attack_conduct;
static FixedBitVector<MAX_MONSTERS> _first_attack_was_unchivalric;
static FixedBitVector<MAX_MONSTERS> _first_attack_was_friendly;

void god_conduct_turn_start()
{
    _first_attack_conduct.reset();
    _first_attack_was_unchivalric.reset();
    _first_attack_was_friendly.reset();
}

void set_attack_conducts(god_conduct_trigger conduct[3], const monster* mon,
                         bool known)
{
    const unsigned int midx = mon->mindex();

    if (mon->friendly())
    {
        if (!_first_attack_conduct[midx]
            || _first_attack_was_friendly[midx])
        {
            conduct[0].set(DID_ATTACK_FRIEND, 5, known, mon);
            _first_attack_was_friendly.set(midx);
        }
    }
    else if (mon->neutral())
        conduct[0].set(DID_ATTACK_NEUTRAL, 5, known, mon);

    if (find_stab_type(&you, mon) != STAB_NO_STAB
        && (!_first_attack_conduct[midx]
            || _first_attack_was_unchivalric[midx]))
    {
        conduct[1].set(DID_UNCHIVALRIC_ATTACK, 4, known, mon);
        _first_attack_was_unchivalric.set(midx);
    }

    if (mon->is_holy() && !mon->is_illusion())
        conduct[2].set(DID_ATTACK_HOLY, mon->get_experience_level(), known, mon);

    _first_attack_conduct.set(midx);
}

void enable_attack_conducts(god_conduct_trigger conduct[3])
{
    for (int i = 0; i < 3; ++i)
        conduct[i].enabled = true;
}

void disable_attack_conducts(god_conduct_trigger conduct[3])
{
    for (int i = 0; i < 3; ++i)
        conduct[i].enabled = false;
}
