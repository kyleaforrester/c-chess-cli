/*
 * c-chess-cli, a command line interface for UCI chess engines. Copyright 2020 lucasart.
 *
 * c-chess-cli is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * c-chess-cli is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If
 * not, see <http://www.gnu.org/licenses/>.
*/
#include <ctype.h>
#include <stdio.h>
#include "position.h"
#include "util.h"

static const char *PieceLabel[NB_COLOR] = {"NBRQKP.", "nbrqkp."};

static uint64_t ZobristKey[NB_COLOR][NB_PIECE][NB_SQUARE];
static uint64_t ZobristCastling[NB_SQUARE], ZobristEnPassant[NB_SQUARE + 1], ZobristTurn;

static __attribute__((constructor)) void zobrist_init(void)
{
    uint64_t state = 0;

    for (int color = WHITE; color <= BLACK; color++)
        for (int piece = KNIGHT; piece < NB_PIECE; piece++)
            for (int square = A1; square <= H8; square++)
                ZobristKey[color][piece][square] = prng(&state);

    for (int square = A1; square <= H8; square++) {
        ZobristCastling[square] = prng(&state);
        ZobristEnPassant[square] = prng(&state);
    }

    ZobristEnPassant[NB_SQUARE] = prng(&state);
    ZobristTurn = prng(&state);
}

static uint64_t zobrist_castling(bitboard_t castleRooks)
{
    bitboard_t k = 0;

    while (castleRooks)
        k ^= ZobristCastling[bb_pop_lsb(&castleRooks)];

    return k;
}

static void square_to_string(int square, char str[3])
{
    BOUNDS(square, NB_SQUARE + 1);

    if (square == NB_SQUARE)
        *str++ = '-';
    else {
        *str++ = (char)file_of(square) + 'a';
        *str++ = (char)rank_of(square) + '1';
    }

    *str = '\0';
}

static int string_to_square(str_t str)
{
    return str.buf[0] != '-'
        ? square_from(str.buf[1] - '1', str.buf[0] - 'a')
        : NB_SQUARE;
}

// Remove 'piece' of 'color' on 'square'. Such a piece must be there first.
static void clear_square(Position *pos, int color, int piece, int square)
{
    BOUNDS(color, NB_COLOR);
    BOUNDS(piece, NB_PIECE);
    BOUNDS(square, NB_SQUARE);

    bb_clear(&pos->byColor[color], square);
    bb_clear(&pos->byPiece[piece], square);
    pos->key ^= ZobristKey[color][piece][square];
}

// Put 'piece' of 'color' on 'square'. Square must be empty first.
static void set_square(Position *pos, int color, int piece, int square)
{
    BOUNDS(color, NB_COLOR);
    BOUNDS(piece, NB_PIECE);
    BOUNDS(square, NB_SQUARE);

    bb_set(&pos->byColor[color], square);
    bb_set(&pos->byPiece[piece], square);
    pos->key ^= ZobristKey[color][piece][square];
}

static void finish(Position *pos)
{
    const int us = pos->turn, them = opposite(us);
    const int king = pos_king_square(pos, us);

    // ** Calculate pos->pins **

    pos->pins = 0;
    bitboard_t pinners = (pos_pieces_cpp(pos, them, ROOK, QUEEN) & bb_rook_attacks(king, 0))
        | (pos_pieces_cpp(pos, them, BISHOP, QUEEN) & bb_bishop_attacks(king, 0));

    while (pinners) {
        const int pinner = bb_pop_lsb(&pinners);
        bitboard_t skewered = Segment[king][pinner] & pos_pieces(pos);
        bb_clear(&skewered, king);
        bb_clear(&skewered, pinner);

        if (!bb_several(skewered) && (skewered & pos->byColor[us]))
            pos->pins |= skewered;
    }

    // ** Calculate pos->attacked **

    // King and Knight attacks
    pos->attacked = KingAttacks[pos_king_square(pos, them)];
    bitboard_t knights = pos_pieces_cp(pos, them, KNIGHT);

    while (knights)
        pos->attacked |= KnightAttacks[bb_pop_lsb(&knights)];

    // Pawn captures
    bitboard_t pawns = pos_pieces_cp(pos, them, PAWN);
    pos->attacked |= bb_shift(pawns & ~File[FILE_A], push_inc(them) + LEFT);
    pos->attacked |= bb_shift(pawns & ~File[FILE_H], push_inc(them) + RIGHT);

    // Sliders (using modified occupancy to see through a checked king)
    bitboard_t occ = pos_pieces(pos) ^ pos_pieces_cp(pos, opposite(them), KING);
    bitboard_t rookMovers = pos_pieces_cpp(pos, them, ROOK, QUEEN);

    while (rookMovers)
        pos->attacked |= bb_rook_attacks(bb_pop_lsb(&rookMovers), occ);

    bitboard_t bishopMovers = pos_pieces_cpp(pos, them, BISHOP, QUEEN);

    while (bishopMovers)
        pos->attacked |= bb_bishop_attacks(bb_pop_lsb(&bishopMovers), occ);

    // ** Calculate pos->checkers **

    if (bb_test(pos->attacked, king)) {
        pos->checkers = (pos_pieces_cp(pos, them, PAWN) & PawnAttacks[us][king])
            | (pos_pieces_cp(pos, them, KNIGHT) & KnightAttacks[king])
            | (pos_pieces_cpp(pos, them, ROOK, QUEEN) & bb_rook_attacks(king, pos_pieces(pos)))
            | (pos_pieces_cpp(pos, them, BISHOP, QUEEN) & bb_bishop_attacks(king, pos_pieces(pos)));

        // We can't be checked by the opponent king
        assert(!(pos_pieces_cp(pos, them, KING) & KingAttacks[king]));

        // Since our king is attacked, we must have at least one checker. Also more than 2 checkers
        // is impossible (even in Chess960).
        assert(pos->checkers && bb_count(pos->checkers) <= 2);
    } else
        pos->checkers = 0;
}

static bool pos_move_is_capture(const Position *pos, move_t m)
// Detect normal captures only (not en passant)
{
    return bb_test(pos->byColor[opposite(pos->turn)], move_to(m));
}

// Set position from FEN string
bool pos_set(Position *pos, str_t fen, bool chess960)
{
    *pos = (Position){0};
    scope(str_del) str_t token = {0};

    // Piece placement
    const char *tail = str_tok(fen.buf, &token, " ");
    int file = FILE_A, rank = RANK_8;

    for (const char *c = token.buf; *c; c++) {
        if ('1' <= *c && *c <= '8') {
            file += *c -'0';

            if (file > NB_FILE)
                return false;
        } else if (*c == '/') {
            rank--;
            file = FILE_A;
        } else {
            if (!strchr("nbrqkpNBRQKP", *c))
                return false;

            const bool color = islower((unsigned)*c);
            set_square(pos, color, (int)(strchr(PieceLabel[color], *c) - PieceLabel[color]),
                square_from(rank, file++));
        }
    }

    if (rank != RANK_1)
        return false;

    // Turn of play
    tail = str_tok(tail, &token, " ");

    if (token.len != 1)
        return false;

    if (token.buf[0] == 'w')
        pos->turn = WHITE;
    else {
        if (token.buf[0] != 'b')
            return false;

        pos->turn = BLACK;
        pos->key ^= ZobristTurn;
    }

    // Castling rights: optional, default '-'
    if ((tail = str_tok(tail, &token, " "))) {
        if (token.len > 4)
            return false;

        for (const char *c = token.buf; *c; c++) {
            rank = isupper((unsigned)*c) ? RANK_1 : RANK_8;
            char uc = (char)toupper(*c);

            if (uc == 'K')
                bb_set(&pos->castleRooks, bb_msb(Rank[rank] & pos->byPiece[ROOK]));
            else if (uc == 'Q')
                bb_set(&pos->castleRooks, bb_lsb(Rank[rank] & pos->byPiece[ROOK]));
            else if ('A' <= uc && uc <= 'H')
                bb_set(&pos->castleRooks, square_from(rank, uc - 'A'));
            else if (*c != '-' || pos->castleRooks || c[1] != '\0')
                return false;
        }
    }

    pos->key ^= zobrist_castling(pos->castleRooks);

    // Chess960
    pos->chess960 = chess960;
    // TODO: if (pos_need_chess960(pos) && !chess960) return false;

    // En passant square: optional, default '-'
    if (!(tail = str_tok(tail, &token, " ")))
        str_cpy(&token, str_ref("-"));

    if (token.len > 2)
        return false;

    pos->epSquare = (uint8_t)string_to_square(token);
    pos->key ^= ZobristEnPassant[pos->epSquare];

    // 50 move counter (in plies, starts at 0): optional, default 0
    pos->rule50 = (tail = str_tok(tail, &token, " ")) ? (uint8_t)atoi(token.buf) : 0;

    if (pos->rule50 >= 100)
        return false;

    // Full move counter (in moves, starts at 1): optional, default 1
    pos->fullMove = str_tok(tail, &token, " ") ? (uint16_t)atoi(token.buf) : 1;

    // Verify piece counts
    for (int color = WHITE; color <= BLACK; color++)
        if (bb_count(pos_pieces_cpp(pos, color, KNIGHT, PAWN)) > 10
                || bb_count(pos_pieces_cpp(pos, color, BISHOP, PAWN)) > 10
                || bb_count(pos_pieces_cpp(pos, color, ROOK, PAWN)) > 10
                || bb_count(pos_pieces_cpp(pos, color, QUEEN, PAWN)) > 9
                || bb_count(pos_pieces_cp(pos, color, PAWN)) > 8
                || bb_count(pos_pieces_cp(pos, color, KING)) != 1
                || bb_count(pos->byColor[color]) > 16)
            return false;

    // Verify pawn ranks
    if (pos->byPiece[PAWN] & (Rank[RANK_1] | Rank[RANK_8]))
        return false;

    // Verify castle rooks
    if (pos->castleRooks) {
        if (pos->castleRooks & ~((Rank[RANK_1] & pos_pieces_cp(pos, WHITE, ROOK))
                | (Rank[RANK_8] & pos_pieces_cp(pos, BLACK, ROOK))))
            return false;

        for (int color = WHITE; color <= BLACK; color++) {
            const bitboard_t b = pos->castleRooks & pos->byColor[color];

            if (bb_count(b) == 2) {
                if (!(Segment[bb_lsb(b)][bb_msb(b)] & pos_pieces_cp(pos, color, KING)))
                    return false;
            } else if (bb_count(b) == 1) {
                if (pos_pieces_cp(pos, color, KING) & (File[FILE_A] | File[FILE_H]))
                    return false;
            } else if (b) {
                assert(bb_count(b) >= 3);
                return false;
            }
        }
    }

    // Verify ep square
    if (pos->epSquare != NB_SQUARE) {
        rank = rank_of(pos->epSquare);
        const int color = rank == RANK_3 ? WHITE : BLACK;

        if ((color == pos->turn)
                || (bb_test(pos_pieces(pos), pos->epSquare))
                || (rank != RANK_3 && rank != RANK_6)
                || (!bb_test(pos_pieces_cp(pos, color, PAWN), pos->epSquare + push_inc(color)))
                || (bb_test(pos_pieces(pos), pos->epSquare - push_inc(color))))
            return false;
    }

    finish(pos);
    return true;
}

// Get FEN string of position
str_t pos_get(const Position *pos)
{
    str_t fen = {0};

    // Piece placement
    for (int rank = RANK_8; rank >= RANK_1; rank--) {
        int cnt = 0;

        for (int file = FILE_A; file <= FILE_H; file++) {
            const int square = square_from(rank, file);

            if (bb_test(pos_pieces(pos), square)) {
                if (cnt)
                    str_push(&fen, (char)cnt + '0');

                str_push(&fen, PieceLabel[pos_color_on(pos, square)][pos_piece_on(pos, square)]);
                cnt = 0;
            } else
                cnt++;
        }

        if (cnt)
            str_push(&fen, (char)cnt + '0');

        str_push(&fen, rank == RANK_1 ? ' ' : '/');
    }

    // Turn of play
    str_cat(&fen, str_ref(pos->turn == WHITE ? "w " : "b "));

    // Castling rights
    if (!pos->castleRooks)
        str_push(&fen, '-');
    else {
        for (int color = WHITE; color <= BLACK; color++) {
            const bitboard_t b = pos->castleRooks & pos->byColor[color];

            if (b) {
                const int king = pos_king_square(pos, color);

                // Right side castling
                if (b & Ray[king][king + RIGHT])
                    str_push(&fen, PieceLabel[color][KING]);

                // Left side castling
                if (b & Ray[king][king + LEFT])
                    str_push(&fen, PieceLabel[color][QUEEN]);
            }
        }
    }

    // En passant and 50 move
    char epStr[3];
    square_to_string(pos->epSquare, epStr);
    str_cat_fmt(&fen, " %s %i %i", epStr, pos->rule50, pos->fullMove);

    return fen;
}

// Play a move on a position copy (original 'before' is untouched): pos = before + play(m)
void pos_move(Position *pos, const Position *before, move_t m)
{
    *pos = *before;

    pos->rule50++;
    pos->epSquare = NB_SQUARE;

    const int us = pos->turn, them = opposite(us);
    const int from = move_from(m), to = move_to(m), prom = move_prom(m);
    const int piece = pos_piece_on(pos, from);
    const int capture = pos_piece_on(pos, to);

    // Capture piece on to square (if any)
    if (capture != NB_PIECE) {
        assert(capture != KING);
        assert(!bb_test(pos->byColor[us], to) || (bb_test(pos->castleRooks, to) && piece == KING));
        pos->rule50 = 0;

        // Use pos_color_on() instead of them, because we could be playing a KxR castling here
        clear_square(pos, pos_color_on(pos, to), capture, to);

        // Capturing a rook alters corresponding castling right
        pos->castleRooks &= ~(1ULL << to);
    }

    if (piece <= QUEEN) {
        // Move piece
        clear_square(pos, us, piece, from);
        set_square(pos, us, piece, to);

        // Lose specific castling right (if not already lost)
        pos->castleRooks &= ~(1ULL << from);
    } else {
        // Move piece
        clear_square(pos, us, piece, from);
        set_square(pos, us, piece, to);

        if (piece == PAWN) {
            // reset rule50, and set epSquare
            const int push = push_inc(us);
            pos->rule50 = 0;

            // Set ep square upon double push, only if catpturably by enemy pawns
            if (to == from + 2 * push
                    && (PawnAttacks[us][from + push] & pos_pieces_cp(pos, them, PAWN)))
                pos->epSquare = (uint8_t)(from + push);

            // handle ep-capture and promotion
            if (to == before->epSquare)
                clear_square(pos, them, piece, to - push);
            else if (rank_of(to) == RANK_8 || rank_of(to) == RANK_1) {
                clear_square(pos, us, piece, to);
                set_square(pos, us, prom, to);
            }
        } else if (piece == KING) {
            // Lose all castling rights
            pos->castleRooks &= ~Rank[us * RANK_8];

            // Castling
            if (bb_test(before->byColor[us], to)) {
                // Capturing our own piece can only be a castling move, encoded KxR
                assert(pos_piece_on(before, to) == ROOK);
                const int rank = rank_of(from);

                clear_square(pos, us, KING, to);
                set_square(pos, us, KING, square_from(rank, to > from ? FILE_G : FILE_C));
                set_square(pos, us, ROOK, square_from(rank, to > from ? FILE_F : FILE_D));
            }
        }
    }

    pos->turn = (uint8_t)them;
    pos->key ^= ZobristTurn;
    pos->key ^= ZobristEnPassant[before->epSquare] ^ ZobristEnPassant[pos->epSquare];
    pos->key ^= zobrist_castling(before->castleRooks ^ pos->castleRooks);
    pos->fullMove += pos->turn == WHITE;
    pos->lastMove = m;

    finish(pos);
}

// All pieces
bitboard_t pos_pieces(const Position *pos)
{
    assert(!(pos->byColor[WHITE] & pos->byColor[BLACK]));
    return pos->byColor[WHITE] | pos->byColor[BLACK];
}

// Pieces of color 'color' and type 'piece'
bitboard_t pos_pieces_cp(const Position *pos, int color, int piece)
{
    BOUNDS(color, NB_COLOR);
    BOUNDS(piece, NB_PIECE);
    return pos->byColor[color] & pos->byPiece[piece];
}

// Pieces of color 'color' and type 'p1' or 'p2'
bitboard_t pos_pieces_cpp(const Position *pos, int color, int p1, int p2)
{
    BOUNDS(color, NB_COLOR);
    BOUNDS(p1, NB_PIECE);
    BOUNDS(p2, NB_PIECE);
    return pos->byColor[color] & (pos->byPiece[p1] | pos->byPiece[p2]);
}

// Detect insufficient material configuration (draw by chess rules only)
bool pos_insufficient_material(const Position *pos)
{
    return bb_count(pos_pieces(pos)) <= 3 && !pos->byPiece[PAWN] && !pos->byPiece[ROOK]
        && !pos->byPiece[QUEEN];
}

// Square occupied by the king of color 'color'
int pos_king_square(const Position *pos, int color)
{
    assert(bb_count(pos_pieces_cp(pos, color, KING)) == 1);
    return bb_lsb(pos_pieces_cp(pos, color, KING));
}

// Color of piece on square 'square'. Square is assumed to be occupied.
int pos_color_on(const Position *pos, int square)
{
    assert(bb_test(pos_pieces(pos), square));
    return bb_test(pos->byColor[WHITE], square) ? WHITE : BLACK;
}

// Piece on square 'square'. NB_PIECE if empty.
int pos_piece_on(const Position *pos, int square)
{
    BOUNDS(square, NB_SQUARE);

    int piece;

    for (piece = KNIGHT; piece <= PAWN; piece++)
        if (bb_test(pos->byPiece[piece], square))
            break;

    return piece;
}

bool pos_move_is_castling(const Position *pos, move_t m)
{
    return bb_test(pos->byColor[pos->turn], move_to(m));
}

str_t *pos_move_to_lan(const Position *pos, move_t m, str_t *out)
{
    const int from = move_from(m), prom = move_prom(m);
    int to = move_to(m);

    if (!(from | to | prom)) {
        str_cat(out, str_ref("0000"));
        return out;
    }

    if (!pos->chess960 && pos_move_is_castling(pos, m))
        to = to > from ? from + 2 : from - 2;  // e1h1 -> e1g1, e1a1 -> e1c1

    char fromStr[3], toStr[3];
    square_to_string(from, fromStr);
    square_to_string(to, toStr);
    str_cat(str_cat(out, str_ref(fromStr)), str_ref(toStr));

    if (prom < NB_PIECE)
        str_push(out, PieceLabel[BLACK][prom]);

    return out;
}

move_t pos_lan_to_move(const Position *pos, str_t lan)
{
    const int prom = lan.buf[4]
        ? (int)(strchr(PieceLabel[BLACK], lan.buf[4]) - PieceLabel[BLACK])
        : NB_PIECE;
    const int from = square_from(lan.buf[1] - '1', lan.buf[0] - 'a');
    int to = square_from(lan.buf[3] - '1', lan.buf[2] - 'a');

    if (!pos->chess960 && pos_piece_on(pos, from) == KING) {
        if (to == from + 2)  // e1g1 -> e1h1
            to++;
        else if (to == from - 2)  // e1c1 -> e1a1
            to -= 2;
    }

    return move_build(from, to, prom);
}

str_t *pos_move_to_san(const Position *pos, move_t m, str_t *out)
// Converts a move to Standard Algebraic Notation. Note that the '+' (check) or '#' (checkmate)
// suffixes are not generated here.
{
    const int us = pos->turn;
    const int from = move_from(m), to = move_to(m), prom = move_prom(m);
    const int piece = pos_piece_on(pos, from);

    if (piece == PAWN) {
        str_push(out, (char)file_of(from) + 'a');

        if (pos_move_is_capture(pos, m) || to == pos->epSquare)
            str_push(str_push(out, 'x'), (char)file_of(to) + 'a');

        str_push(out, (char)rank_of(to) + '1');

        if (prom < NB_PIECE)
            str_push(str_push(out, '='), PieceLabel[WHITE][prom]);
    } else if (piece == KING) {
        if (pos_move_is_castling(pos, m))
            str_cat(out, str_ref(to > from ? "O-O" : "O-O-O"));
        else {
            str_push(out, 'K');

            if (pos_move_is_capture(pos, m))
                str_push(out, 'x');

            char toStr[3];
            square_to_string(to, toStr);
            str_cat(out, str_ref(toStr));
        }
    } else {
        str_push(out, PieceLabel[WHITE][piece]);

        // ** SAN disambiguation **

        // 1. Build a list of 'contesters', which are all our pieces of the same type that can also
        // reach the 'to' square.
        const bitboard_t pins = pos->pins;
        bitboard_t contesters = pos_pieces_cp(pos, us, piece);
        bb_clear(&contesters, from);

        if (piece == KNIGHT)
            // 1.1. Knights. Restrict to those within a knight jump of of 'to' that are not pinned.
            contesters &= KnightAttacks[to] & ~pins;
        else {
            // 1.2. Sliders
            assert(BISHOP <= piece && piece <= QUEEN);

            // 1.2.1. Restrict to those that can pseudo-legally reach the 'to' square.
            bitboard_t occ = pos_pieces(pos);

            if (piece == BISHOP)
                contesters &= bb_bishop_attacks(to, occ);
            else if (piece == ROOK)
                contesters &= bb_rook_attacks(to, occ);
            else if (piece == QUEEN)
                contesters &= bb_bishop_attacks(to, occ) | bb_rook_attacks(to, occ);

            // 1.2.2. Remove pinned sliders, which, by sliding to the 'to' square, would escape
            // their pin-ray.
            bitboard_t pinnedContesters = contesters & pins;

            while (pinnedContesters) {
                const int pinnedContester = bb_pop_lsb(&pinnedContesters);

                if (!bb_test(Ray[pos_king_square(pos, us)][pinnedContester], to))
                    bb_clear(&contesters, pinnedContester);
            }
        }

        // 2. Use the contesters to disambiguate
        if (contesters) {
            // 2.1. Same file or rank, use either or both to disambiguate.
            if (bb_rook_attacks(from, 0) & contesters) {
                // 2.1.1. Contested rank. Use file to disambiguate
                if (Rank[rank_of(from)] & contesters)
                    str_push(out, (char)file_of(from) + 'a');

                // 2.1.2. Contested file. Use rank to disambiguate
                if (File[file_of(from)] & contesters)
                    str_push(out, (char)rank_of(from) + '1');
            } else
                // 2.2. No file or rank in common, use file to disambiguate.
                str_push(out, (char)file_of(from) + 'a');
        }

        if (pos_move_is_capture(pos, m))
            str_push(out, 'x');

        char toStr[3];
        square_to_string(to, toStr);
        str_cat(out, str_ref(toStr));
    }

    return out;
}

// Prints the position in ASCII 'art' (for debugging)
void pos_print(const Position *pos)
{
    for (int rank = RANK_8; rank >= RANK_1; rank--) {
        char line[] = ". . . . . . . .";

        for (int file = FILE_A; file <= FILE_H; file++) {
            const int square = square_from(rank, file);
            line[2 * file] = bb_test(pos_pieces(pos), square)
                ? PieceLabel[pos_color_on(pos, square)][pos_piece_on(pos, square)]
                : square == pos->epSquare ? '*' : '.';
        }

        puts(line);
    }

    scope(str_del) str_t fen = pos_get(pos);
    puts(fen.buf);

    scope(str_del) str_t msg = str_dup(str_ref("Last move: "));
    pos_move_to_lan(pos, pos->lastMove, &msg);
    puts(msg.buf);
}
