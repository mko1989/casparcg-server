#version 450
in vec4 TexCoordIn;
in vec2 Position;
in vec2 LocalIn;    // slices: quad-local coordinates (x q)
in vec4 FeatherIn;  // slices: feather l t r b in quad-local units
in float OpacityIn; // slices: opacity
in vec4 SrcClampIn; // slices: source rect in texture space, inset by half a texel

out vec4 TexCoord;
out vec2 Local;
flat out vec4 Feather;
flat out float Opacity;
flat out vec4 SrcClamp;

void main()
{
    TexCoord = TexCoordIn;
    Local = LocalIn;
    Feather = FeatherIn;
    Opacity = OpacityIn;
    SrcClamp = SrcClampIn;
    gl_Position = vec4(Position, 0, 1);
}
