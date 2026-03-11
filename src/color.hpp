#pragma once
#include <array>

class color
{
    // raw array to pass back to caller representing this color
    std::array<unsigned char, 4> buf;

public:
    // construct from components (also default constructor)
    explicit constexpr color(unsigned char red = 0xFF, unsigned char green = 0xFF, unsigned char blue = 0xFF, unsigned char alpha = 0xFF) throw() : buf { { red, green, blue, alpha } } {}

    // copy constructors
    constexpr color(const color& other, unsigned char alpha = 0xFF) throw() : buf { { other.buf[0], other.buf[1], other.buf[2], alpha } } {}

    // assignment operator
    color& operator=(const color& other) = default;

    // construct from hash
    static constexpr color from_hash(std::size_t hash, unsigned char alpha = 0xFF)
    {
        return color { static_cast<unsigned char>(((hash & 0x001FC000) >> 14) | 0x80),
                       static_cast<unsigned char>(((hash & 0x001FC000) >> 7) | 0x80),
                       static_cast<unsigned char>(((hash & 0x001FC000) >> 0) | 0x80), alpha };
    }

    // return a color suitable to pass to glColor3ubv() or glColor4ubv()
    const unsigned char* buffer() const;

    // return components suitable to pass to C4F vertex buffers and glClearColor()
    float r_f() const;
    float g_f() const;
    float b_f() const;
    float a_f() const;

    // return components suitable to pass to C4UB vertex buffers
    unsigned char r() const;
    unsigned char g() const;
    unsigned char b() const;
    unsigned char a() const;

    // linear interpolation between two colors
    static color lerp(const color& c0, const color& c1, float factor1 = 0.5F);

    // darken color
    color operator*(float factor) const;

    // transparent black at various percentages
    static const color clear;
    static const color black25;
    static const color black50;
    static const color black75;

    // from SVG 1.0 / CSS 3 named color list http://www.w3.org/TR/SVG/types.html#ColorKeywords
    static const color aliceblue;
    static const color antiquewhite;
    static const color aqua;
    static const color aquamarine;
    static const color azure;
    static const color beige;
    static const color bisque;
    static const color black;
    static const color blanchedalmond;
    static const color blue;
    static const color blueviolet;
    static const color brown;
    static const color burlywood;
    static const color cadetblue;
    static const color chartreuse;
    static const color chocolate;
    static const color coral;
    static const color cornflowerblue;
    static const color cornsilk;
    static const color crimson;
    static const color cyan;
    static const color darkblue;
    static const color darkcyan;
    static const color darkgoldenrod;
    static const color darkgray;
    static const color darkgrey;
    static const color darkgreen;
    static const color darkkhaki;
    static const color darkmagenta;
    static const color darkolivegreen;
    static const color darkorange;
    static const color darkorchid;
    static const color darkred;
    static const color darksalmon;
    static const color darkseagreen;
    static const color darkslateblue;
    static const color darkslategray;
    static const color darkslategrey;
    static const color darkturquoise;
    static const color darkviolet;
    static const color deeppink;
    static const color deepskyblue;
    static const color dimgray;
    static const color dimgrey;
    static const color dodgerblue;
    static const color firebrick;
    static const color floralwhite;
    static const color forestgreen;
    static const color fuchsia;
    static const color gainsboro;
    static const color ghostwhite;
    static const color gold;
    static const color goldenrod;
    static const color gray;
    static const color grey;
    static const color green;
    static const color greenyellow;
    static const color honeydew;
    static const color hotpink;
    static const color indianred;
    static const color indigo;
    static const color ivory;
    static const color khaki;
    static const color lavender;
    static const color lavenderblush;
    static const color lawngreen;
    static const color lemonchiffon;
    static const color lightblue;
    static const color lightcoral;
    static const color lightcyan;
    static const color lightgoldenrodyellow;
    static const color lightgray;
    static const color lightgrey;
    static const color lightgreen;
    static const color lightpink;
    static const color lightsalmon;
    static const color lightseagreen;
    static const color lightskyblue;
    static const color lightslategray;
    static const color lightslategrey;
    static const color lightsteelblue;
    static const color lightyellow;
    static const color lime;
    static const color limegreen;
    static const color linen;
    static const color magenta;
    static const color maroon;
    static const color mediumaquamarine;
    static const color mediumblue;
    static const color mediumorchid;
    static const color mediumpurple;
    static const color mediumseagreen;
    static const color mediumslateblue;
    static const color mediumspringgreen;
    static const color mediumturquoise;
    static const color mediumvioletred;
    static const color midnightblue;
    static const color mintcream;
    static const color mistyrose;
    static const color moccasin;
    static const color navajowhite;
    static const color navy;
    static const color oldlace;
    static const color olive;
    static const color olivedrab;
    static const color orange;
    static const color orangered;
    static const color orchid;
    static const color palegoldenrod;
    static const color palegreen;
    static const color paleturquoise;
    static const color palevioletred;
    static const color papayawhip;
    static const color peachpuff;
    static const color peru;
    static const color pink;
    static const color plum;
    static const color powderblue;
    static const color purple;
    static const color rebeccapurple;
    static const color red;
    static const color rosybrown;
    static const color royalblue;
    static const color saddlebrown;
    static const color salmon;
    static const color sandybrown;
    static const color seagreen;
    static const color seashell;
    static const color sienna;
    static const color silver;
    static const color skyblue;
    static const color slateblue;
    static const color slategray;
    static const color slategrey;
    static const color snow;
    static const color springgreen;
    static const color steelblue;
    static const color tan;
    static const color teal;
    static const color thistle;
    static const color tomato;
    static const color turquoise;
    static const color violet;
    static const color wheat;
    static const color white;
    static const color whitesmoke;
    static const color yellow;
    static const color yellowgreen;
};
