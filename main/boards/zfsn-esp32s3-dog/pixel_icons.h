#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Small native monochrome assets, independent of fonts and external packages.
// A set bit is the foreground; the display supplies the panel's logical palette.
namespace DogPixelIcons {
enum class Icon : uint8_t {
    Chat, Emotion, Music, Alarm, Settings, About, Volume, Boot, Charging,
    Explore, Network, Play, Next, Previous, Stop, Calendar, Toggle, Chip, Dog, Back,
};

struct Bitmap {
    std::array<uint8_t, 72> pixels = {};
    constexpr void Dot(int x, int y) {
        if (x >= 0 && x < 24 && y >= 0 && y < 24) pixels[y * 3 + x / 8] |= uint8_t(0x80 >> (x % 8));
    }
    constexpr void Line(int x0, int y0, int x1, int y1, int stroke = 2) {
        const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
        const int dy = y1 > y0 ? y1 - y0 : y0 - y1;
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;
        while (true) {
            for (int y = 0; y < stroke; ++y) for (int x = 0; x < stroke; ++x) Dot(x0 + x, y0 + y);
            if (x0 == x1 && y0 == y1) break;
            const int twice = err * 2;
            if (twice > -dy) { err -= dy; x0 += sx; }
            if (twice < dx) { err += dx; y0 += sy; }
        }
    }
    constexpr void Box(int x, int y, int w, int h) {
        Line(x, y, x + w - 2, y);
        Line(x, y + h - 2, x + w - 2, y + h - 2);
        Line(x, y, x, y + h - 2);
        Line(x + w - 2, y, x + w - 2, y + h - 2);
    }
    constexpr void Fill(int x, int y, int w, int h) {
        for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) Dot(xx, yy);
    }
    constexpr void Ring(int cx, int cy, int radius) {
        for (int y = -radius; y <= radius; ++y) for (int x = -radius; x <= radius; ++x) {
            const int d = x * x + y * y;
            if (d <= radius * radius && d >= (radius - 2) * (radius - 2)) Dot(cx + x, cy + y);
        }
    }
    constexpr void Heart(int x, int y) {
        Fill(x, y, 2, 2); Fill(x + 3, y, 2, 2);
        Fill(x, y + 2, 5, 2); Fill(x + 1, y + 4, 3, 1); Dot(x + 2, y + 5);
    }
};

constexpr Bitmap Draw(Icon icon) {
    Bitmap b;
    switch (icon) {
    case Icon::Chat:
        b.Box(2, 3, 20, 15); b.Line(5, 17, 5, 21); b.Line(5, 21, 10, 17);
        b.Fill(6, 9, 2, 2); b.Fill(11, 9, 2, 2); b.Fill(16, 9, 2, 2); break;
    case Icon::Emotion:
        b.Ring(11, 11, 10); b.Fill(7, 7, 2, 3); b.Fill(15, 7, 2, 3);
        b.Line(6, 14, 9, 17); b.Line(9, 17, 14, 17); b.Line(14, 17, 17, 14); break;
    case Icon::Music:
        b.Line(8, 5, 20, 2); b.Line(8, 5, 8, 18); b.Line(20, 2, 20, 15);
        b.Fill(3, 17, 6, 5); b.Fill(15, 14, 6, 5); b.Line(9, 8, 20, 5); break;
    case Icon::Alarm:
        b.Ring(11, 12, 8); b.Line(11, 7, 11, 12); b.Line(11, 12, 15, 14);
        b.Line(2, 5, 6, 2); b.Line(17, 2, 21, 5); b.Line(6, 20, 4, 22); b.Line(17, 20, 19, 22); break;
    case Icon::Settings:
        b.Ring(11, 11, 8); b.Ring(11, 11, 3);
        b.Fill(9, 0, 5, 4); b.Fill(9, 19, 5, 4); b.Fill(0, 9, 4, 5); b.Fill(19, 9, 4, 5);
        b.Line(3, 3, 5, 5); b.Line(18, 3, 16, 5); b.Line(3, 18, 5, 16); b.Line(18, 18, 16, 16); break;
    case Icon::About:
        b.Ring(11, 11, 10); b.Fill(10, 5, 3, 3); b.Fill(10, 10, 3, 8); b.Line(8, 18, 15, 18); break;
    case Icon::Volume:
        b.Box(2, 9, 5, 7); b.Line(6, 9, 12, 4); b.Line(6, 14, 12, 19); b.Line(12, 4, 12, 19);
        b.Line(16, 7, 19, 11); b.Line(19, 11, 16, 16); b.Line(19, 3, 22, 9); b.Line(22, 9, 22, 14); b.Line(22, 14, 19, 20); break;
    case Icon::Boot:
        b.Line(11, 1, 11, 8); b.Line(11, 15, 11, 22); b.Line(1, 11, 8, 11); b.Line(15, 11, 22, 11);
        b.Line(4, 4, 8, 8); b.Line(15, 15, 19, 19); b.Line(4, 19, 8, 15); b.Line(15, 8, 19, 4); b.Ring(11, 11, 4); break;
    case Icon::Charging:
        b.Box(3, 4, 17, 16); b.Fill(20, 9, 3, 6);
        b.Line(13, 6, 8, 12); b.Line(8, 12, 14, 12); b.Line(14, 12, 9, 18); break;
    case Icon::Explore:
        b.Fill(6, 11, 10, 8); b.Fill(8, 9, 6, 3);
        b.Fill(1, 6, 4, 5); b.Fill(6, 2, 4, 5); b.Fill(12, 2, 4, 5); b.Fill(18, 6, 4, 5); break;
    case Icon::Network:
        b.Line(1, 7, 5, 4); b.Line(5, 4, 17, 4); b.Line(17, 4, 21, 7);
        b.Line(5, 11, 8, 8); b.Line(8, 8, 14, 8); b.Line(14, 8, 17, 11);
        b.Line(8, 15, 11, 12); b.Line(11, 12, 14, 15); b.Fill(10, 18, 3, 3); break;
    case Icon::Play:
        for (int x = 0; x < 14; ++x) {
            b.Line(5 + x, 3 + x / 2, 5 + x, 20 - x / 2, 1);
        }
        break;
    case Icon::Next:
        for (int x = 0; x < 8; ++x) b.Line(3 + x, 4 + x, 3 + x, 19 - x, 1);
        for (int x = 0; x < 8; ++x) b.Line(11 + x, 4 + x, 11 + x, 19 - x, 1);
        b.Fill(20, 4, 2, 16); break;
    case Icon::Previous:
        for (int x = 0; x < 8; ++x) b.Line(20 - x, 4 + x, 20 - x, 19 - x, 1);
        for (int x = 0; x < 8; ++x) b.Line(12 - x, 4 + x, 12 - x, 19 - x, 1);
        b.Fill(2, 4, 2, 16); break;
    case Icon::Stop: b.Fill(5, 5, 14, 14); break;
    case Icon::Calendar:
        b.Box(2, 4, 20, 18); b.Line(2, 9, 20, 9); b.Line(7, 1, 7, 6); b.Line(15, 1, 15, 6);
        b.Fill(6, 13, 3, 3); b.Fill(13, 13, 3, 3); b.Fill(6, 18, 3, 2); b.Fill(13, 18, 3, 2); break;
    case Icon::Toggle:
        b.Box(1, 6, 22, 12); b.Fill(13, 9, 6, 6); break;
    case Icon::Chip:
        b.Box(5, 5, 14, 14); b.Box(9, 9, 6, 6);
        for (int p = 6; p < 19; p += 5) {
            b.Line(p, 1, p, 4); b.Line(p, 19, p, 22); b.Line(1, p, 4, p); b.Line(19, p, 22, p);
        }
        break;
    case Icon::Dog:
        b.Box(6, 5, 13, 15); b.Line(5, 5, 1, 10); b.Line(1, 10, 4, 15); b.Line(18, 5, 22, 10); b.Line(22, 10, 19, 15);
        b.Fill(8, 9, 2, 2); b.Fill(15, 9, 2, 2); b.Fill(11, 13, 3, 3); b.Line(12, 16, 12, 19); break;
    case Icon::Back:
        b.Line(20, 20, 20, 8); b.Line(20, 8, 4, 8);
        b.Line(4, 8, 10, 2); b.Line(4, 8, 10, 14); break;
    }
    return b;
}

constexpr Bitmap DrawEmotion(int index) {
    Bitmap b; b.Ring(11, 11, 10);
    b.Fill(6, 7, 2, 3); b.Fill(15, 7, 2, 3);
    if (index == 3 || index == 12 || index == 19) { b.Fill(5, 7, 4, 2); }
    if (index == 7 || index == 16) { b.Heart(3, 6); b.Heart(14, 6); }
    if (index == 13) { b.Fill(3, 6, 8, 5); b.Fill(13, 6, 8, 5); b.Line(10, 7, 13, 7); }
    if (index == 5) { b.Line(4, 4, 8, 6); b.Line(14, 6, 18, 4); }
    if (index == 6) { b.Line(5, 11, 4, 16); b.Line(17, 11, 18, 16); }
    if (index == 8) { b.Fill(3, 12, 3, 2); b.Fill(18, 12, 3, 2); }
    if (index == 9 || index == 10 || index == 18) b.Ring(11, 16, index == 10 ? 4 : 3);
    else if (index == 4 || index == 5 || index == 6 || index == 20) { b.Line(7, 17, 10, 14); b.Line(10, 14, 13, 14); b.Line(13, 14, 16, 17); }
    else if (index == 0 || index == 11 || index == 17) b.Line(7, 16, 15, 16);
    else { b.Line(6, 14, 9, 17); b.Line(9, 17, 14, 17); b.Line(14, 17, 17, 14); }
    if (index == 2 || index == 19) b.Fill(10, 16, 4, 4);
    if (index == 15) b.Line(16, 14, 18, 12);
    return b;
}

inline constexpr std::array<Bitmap, 20> kIcons = {
    Draw(Icon::Chat), Draw(Icon::Emotion), Draw(Icon::Music), Draw(Icon::Alarm),
    Draw(Icon::Settings), Draw(Icon::About), Draw(Icon::Volume), Draw(Icon::Boot),
    Draw(Icon::Charging), Draw(Icon::Explore), Draw(Icon::Network), Draw(Icon::Play),
    Draw(Icon::Next), Draw(Icon::Previous), Draw(Icon::Stop), Draw(Icon::Calendar),
    Draw(Icon::Toggle), Draw(Icon::Chip), Draw(Icon::Dog), Draw(Icon::Back),
};

inline constexpr std::array<Bitmap, 21> kEmotions = {
    DrawEmotion(0), DrawEmotion(1), DrawEmotion(2), DrawEmotion(3), DrawEmotion(4),
    DrawEmotion(5), DrawEmotion(6), DrawEmotion(7), DrawEmotion(8), DrawEmotion(9),
    DrawEmotion(10), DrawEmotion(11), DrawEmotion(12), DrawEmotion(13), DrawEmotion(14),
    DrawEmotion(15), DrawEmotion(16), DrawEmotion(17), DrawEmotion(18), DrawEmotion(19), DrawEmotion(20),
};
} // namespace DogPixelIcons
