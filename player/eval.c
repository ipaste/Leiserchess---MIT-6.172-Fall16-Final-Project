// Copyright (c) 2015 MIT License by 6.172 Staff

#include "./eval.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "./move_gen.h"
#include "./tbassert.h"
#include "./closebook.h"

// -----------------------------------------------------------------------------
// Evaluation
// -----------------------------------------------------------------------------

#define WINNING_SCORE 30000
typedef int32_t ev_score_t;  // Static evaluator uses "hi res" values

int RANDOMIZE;

int PCENTRAL;
int HATTACK;
int PBETWEEN;
int KFACE;
int KAGGRESSIVE;
int MOBILITY;
int PAWNPIN;

// Heuristics for static evaluation - described in the google doc
// mentioned in the handout.

static const ev_score_t pcentral_s[64] = {
125, 181, 220, 234, 234, 220, 181, 125,
181, 249, 302, 323, 323, 302, 249, 181,
220, 302, 375, 411, 411, 375, 302, 220,
234, 323, 411, 500, 500, 411, 323, 234,
234, 323, 411, 500, 500, 411, 323, 234,
220, 302, 375, 411, 411, 375, 302, 220,
181, 249, 302, 323, 323, 302, 249, 181,
125, 181, 220, 234, 234, 220, 181, 125};

static const uint64_t three_by_three_mask[100] = {
  0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 
  0ULL, 771ULL, 1799ULL, 3598ULL, 7196ULL, 14392ULL, 28784ULL, 57568ULL, 49344ULL, 0ULL, 
  0ULL, 197379ULL, 460551ULL, 921102ULL, 1842204ULL, 3684408ULL, 7368816ULL, 14737632ULL, 12632256ULL, 0ULL, 
  0ULL, 50529024ULL, 117901056ULL, 235802112ULL, 471604224ULL, 943208448ULL, 1886416896ULL, 3772833792ULL, 3233857536ULL, 0ULL, 
  0ULL, 12935430144ULL, 30182670336ULL, 60365340672ULL, 120730681344ULL, 241461362688ULL, 482922725376ULL, 965845450752ULL, 827867529216ULL, 0ULL, 
  0ULL, 3311470116864ULL, 7726763606016ULL, 15453527212032ULL, 30907054424064ULL, 61814108848128ULL, 123628217696256ULL, 247256435392512ULL, 211934087479296ULL, 0ULL, 
  0ULL, 847736349917184ULL, 1978051483140096ULL, 3956102966280192ULL, 7912205932560384ULL, 15824411865120768ULL, 31648823730241536ULL, 63297647460483072ULL, 54255126394699776ULL, 0ULL, 
  0ULL, 217020505578799104ULL, 506381179683864576ULL, 1012762359367729152ULL, 2025524718735458304ULL, 4051049437470916608ULL, 8102098874941833216ULL, 16204197749883666432ULL, 13889312357043142656ULL, 0ULL, 
  0ULL, 217017207043915776ULL, 506373483102470144ULL, 1012746966204940288ULL, 2025493932409880576ULL, 4050987864819761152ULL, 8101975729639522304ULL, 16203951459279044608ULL, 13889101250810609664ULL, 0ULL, 
  0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL};

static const double inv_s[16] = {1.0/1, 1.0/2, 1.0/3, 1.0/4, 1.0/5, 1.0/6, 1.0/7,
1.0/8, 1.0/9, 1.0/10, 1.0/11, 1.0/12, 1.0/13, 1.0/14, 1.0/15, 1.0/16};

// PCENTRAL heuristic: Bonus for Pawn near center of board
#define pcentral(x) pcentral_s[x]

// returns true if c lies on or between a and b, which are not ordered
static inline  bool between(int c, int a, int b) {
  bool x = ((c >= a) && (c <= b)) || ((c <= a) && (c >= b));
  return x;
}

// PBETWEEN heuristic: Bonus for Pawn at (f, r) in rectangle defined by Kings at the corners
// static inline  ev_score_t pbetween(position_t *p, fil_t f, rnk_t r) {
//   bool is_between =
//       between(f, fil_of(p->kloc[WHITE]), fil_of(p->kloc[BLACK])) &&
//       between(r, rnk_of(p->kloc[WHITE]), rnk_of(p->kloc[BLACK]));
//   return is_between ? PBETWEEN : 0;
// }


// KFACE heuristic: bonus (or penalty) for King facing toward the other King
static inline  ev_score_t kface(position_t *p, fil_t f, rnk_t r) {
  square_t sq = square_of(f, r);
  piece_t x = p->board[sq];
  color_t c = color_of(x);
  square_t opp_sq = p->kloc[opp_color(c)];
  int delta_fil = fil_of(opp_sq) - f;
  int delta_rnk = rnk_of(opp_sq) - r;
  int bonus;

  switch (ori_of(x)) {
    case NN:
      bonus = delta_rnk;
      break;

    case EE:
      bonus = delta_fil;
      break;

    case SS:
      bonus = -delta_rnk;
      break;

    case WW:
      bonus = -delta_fil;
      break;

    default:
      bonus = 0;
      tbassert(false, "Illegal King orientation.\n");
  }
  return (bonus * KFACE) * inv_s[abs(delta_rnk) + abs(delta_fil)-1];
}

// KAGGRESSIVE heuristic: bonus for King with more space to back
static inline  ev_score_t kaggressive(position_t *p, fil_t f, rnk_t r) {
  square_t sq = square_of(f, r);
  piece_t x = p->board[sq];
  color_t c = color_of(x);
  tbassert(ptype_of(x) == KING, "ptype_of(x) = %d\n", ptype_of(x));

  square_t opp_sq = p->kloc[opp_color(c)];
  fil_t of = fil_of(opp_sq);
  rnk_t _or = (rnk_t) rnk_of(opp_sq);

  int delta_fil = of - f;
  int delta_rnk = _or - r;

  int bonus = 0;

  if (delta_fil >= 0 && delta_rnk >= 0) {
    bonus = (f + 1) * (r + 1);
  } else if (delta_fil <= 0 && delta_rnk >= 0) {
    bonus = (BOARD_WIDTH - f) * (r + 1);
  } else if (delta_fil <= 0 && delta_rnk <= 0) {
    bonus = (BOARD_WIDTH - f) * (BOARD_WIDTH - r);
  } else if (delta_fil >= 0 && delta_rnk <= 0) {
    bonus = (f + 1) * (BOARD_WIDTH - r);
  }
  return (KAGGRESSIVE * bonus) / (BOARD_WIDTH * BOARD_WIDTH);
}

// Marks the path/line-of-sight of the laser until it hits a piece or goes off
// the board.
//
// p : Current board state.
// c : Color of king shooting laser.
// laser_map : End result will be stored here. Every square on the
//             path of the laser is marked with mark_mask.
// mark_mask : What each square is marked with.

// directions for laser: NN, EE, SS, WW
static const int beam_64[NUM_ORI] = {1, 8, -1, -8};

uint64_t mark_laser_path_bit(position_t *p, color_t c) {
  // static int count = 0;
  // count += 1;
  // if (count % 10000 == 0)
  //   printf("%d\n", count);
  square_t sq = p->kloc[c];
  int loc64 = fil_of(sq) * 8 + rnk_of(sq);
  uint64_t laser_map = 0;
  int bdir = ori_of(p->board[sq]);
  p->kill_d[c] = false;

  tbassert(ptype_of(p->board[sq]) == KING,
           "ptype: %d\n", ptype_of(p->board[sq]));
  laser_map |= (1ULL << loc64);
  
  while (true) {
    sq += beam_of(bdir);
    int x = ptype_of(p->board[sq]);
    if (x == INVALID)
      return laser_map;
    // tbassert(sq < ARR_SIZE && sq >= 0, "sq: %d\n", sq);
    loc64 += beam_64[bdir];
    laser_map |= (1ULL << loc64);
    if (x == KING) {
      p->kill_d[c] = true;
      return laser_map;
    }
    if (x == PAWN) {
      bdir = reflect_of(bdir, ori_of(p->board[sq]));
      if (bdir < 0) {  // Hit back of Pawn
        p->kill_d[c] = true;
        return laser_map;
      }
    }
  }
  return laser_map;
}

// PAWNPIN Heuristic: count number of pawns that are not pinned by the
//   opposing king's laser --- and are thus mobile.

static inline int pawnpin(position_t *p, color_t color, uint64_t laser_map) {
  tbassert(p->mask[0] == compute_mask(p, 0),
           "p->mask: %"PRIu64", mask: %"PRIu64"\n",
           p->mask[0], compute_mask(p, 0));
  tbassert(p->mask[1] == compute_mask(p, 1),
           "p->mask: %"PRIu64", mask: %"PRIu64"\n",
           p->mask[1], compute_mask(p, 1));

  uint64_t mask = (~laser_map) & p -> mask[color];
  return __builtin_popcountl(mask) - ((1ULL << (fil_of(p -> kloc[color]) * 8 + rnk_of(p -> kloc[color])) & mask) != 0);

}

// MOBILITY heuristic: safe squares around king of given color.

static inline int mobility(position_t *p, color_t color, uint64_t laser_map) {
  return __builtin_popcountl((~laser_map) & three_by_three_mask[p -> kloc[color]]);
}


// H_SQUARES_ATTACKABLE heuristic: for shooting the enemy king
static inline int h_squares_attackable(position_t *p, color_t c, uint64_t laser_map) {

  square_t o_king_sq = p->kloc[opp_color(c)];
  tbassert(ptype_of(p->board[o_king_sq]) == KING,
           "ptype: %d\n", ptype_of(p->board[o_king_sq]));
  tbassert(color_of(p->board[o_king_sq]) != c,
           "color: %d\n", color_of(p->board[o_king_sq]));

  float h_attackable = 0;

  int king_sq_fil = fil_of(o_king_sq);
  int king_sq_rnk = rnk_of(o_king_sq);

  while (laser_map) {
    uint64_t y = laser_map & (-laser_map);
    int i = LOG2(y);
    h_attackable += inv_s[abs(king_sq_fil - (i >> 3))] + inv_s[abs(king_sq_rnk - (i & 7))];
    laser_map ^= y;
  }

  return h_attackable;
}

// Static evaluation.  Returns score
score_t eval(position_t *p, bool verbose) {
  // seed rand_r with a value of 1, as per
  // http://linux.die.net/man/3/rand_r

  static __thread unsigned int seed = 1;
  
  int t = checkEndGame(p);
  if (t==1 || t==2)
  {
	  if (t==1) return WINNING_SCORE; else return -WINNING_SCORE;
  }
  

  ev_score_t score = 0;
  
  fil_t f0 = fil_of(p -> kloc[0]);
  rnk_t r0 = rnk_of(p -> kloc[0]);
  fil_t f1 = fil_of(p -> kloc[1]);
  rnk_t r1 = rnk_of(p -> kloc[1]);
  score += kface(p, f0, r0) + kaggressive(p, f0, r0);
  score -= pcentral(f0 * 8 + r0);

  score -= kface(p, f1, r1) + kaggressive(p, f1, r1);
  score += pcentral(f1 * 8 + r1);


  uint64_t mask = p -> mask[0];
  while (mask) {
    uint64_t y = mask & (-mask);
    mask ^= y;
    uint8_t i = LOG2(y);
    fil_t f = (i >> 3);
    rnk_t r = i & 7;
    score += PAWN_EV_VALUE;
    // score += pbetween(p, f, r);
    score += (between(f, f0, f1) && between(r, r0, r1)) ? PBETWEEN : 0;
    // score += pcentral(f, r);
    score += pcentral(i);
  }
  mask = p -> mask[1];
  while (mask) {
    uint64_t y = mask & (-mask);
    mask ^= y;
    uint8_t i = LOG2(y);
    fil_t f = (i >> 3);
    rnk_t r = i & 7;
    score -= PAWN_EV_VALUE;
    // score -= pbetween(p, f, r);
    score -= (between(f, f0, f1) && between(r, r0, r1)) ? PBETWEEN : 0;
    score -= pcentral(i);
  }

  uint64_t laser_WHITE = p -> laser[0];
  uint64_t laser_BLACK = p -> laser[1];
  // if (laser_WHITE != p -> laser[0])
  //   printf("error\n");
  
  // H_SQUARES_ATTACKABLE heuristic
  ev_score_t w_hattackable = HATTACK * h_squares_attackable(p, WHITE, laser_WHITE);
  score += w_hattackable;
  // if (verbose) {
  //   printf("HATTACK bonus %d for White\n", w_hattackable);
  // }
  ev_score_t b_hattackable = HATTACK * h_squares_attackable(p, BLACK, laser_BLACK);
  score -= b_hattackable;
  // if (verbose) {
  //   printf("HATTACK bonus %d for Black\n", b_hattackable);
  // }

  // MOBILITY heuristic
  int w_mobility = MOBILITY * mobility(p, WHITE, laser_BLACK);
  score += w_mobility;
  // if (verbose) {
  //   printf("MOBILITY bonus %d for White\n", w_mobility);
  // }
  int b_mobility = MOBILITY * mobility(p, BLACK, laser_WHITE);
  score -= b_mobility;
  // if (verbose) {
  //   printf("MOBILITY bonus %d for Black\n", b_mobility);
  // }

  // PAWNPIN heuristic --- is a pawn immobilized by the enemy laser.
  int w_pawnpin = PAWNPIN * pawnpin(p, WHITE, laser_BLACK);
  score += w_pawnpin;
  int b_pawnpin = PAWNPIN * pawnpin(p, BLACK, laser_WHITE);
  score -= b_pawnpin;

  // score from WHITE point of view
  // ev_score_t tot = score[WHITE] - score[BLACK];

  if (RANDOMIZE) {
    ev_score_t  z = rand_r(&seed) % (RANDOMIZE*2+1);
    score = score + z - RANDOMIZE;
  }

  if (color_to_move_of(p) == BLACK) {
    score = -score;
  }

  return score / EV_SCORE_RATIO;
}
  
