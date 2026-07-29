/*
Scanlines -- a plain CRT line effect with none of the tube geometry.

Dolphin ships 48 post-processing shaders and not one of them is a scanline
filter (nightvision2scanlines is a nightvision effect), so this is written from
scratch rather than borrowed.

Lines are spaced in SOURCE space, not output space, so the effect does not
change density when the internal resolution or window size changes.

[configuration]

[OptionRangeFloat]
GUIName = Scanline strength
OptionName = STRENGTH
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.35

[OptionRangeFloat]
GUIName = Scanline count
OptionName = LINES
MinValue = 120.0
MaxValue = 1080.0
StepAmount = 30.0
DefaultValue = 480.0

[OptionRangeFloat]
GUIName = Brightness compensation
OptionName = BOOST
MinValue = 1.0
MaxValue = 2.0
StepAmount = 0.05
DefaultValue = 1.20

[/configuration]
*/

void main()
{
	float4 color = Sample();
	float2 uv = GetCoordinates();

	// Position within the current scanline, 0..1.
	float pos = fract(uv.y * GetOption(LINES));

	// Triangular profile: brightest through the middle of the line, darkest at
	// its edges. A hard on/off band aliases badly once the output is not an
	// exact multiple of the line count; this stays smooth at any scale.
	float profile = 1.0 - abs(pos - 0.5) * 2.0;
	float shade = 1.0 - GetOption(STRENGTH) * (1.0 - profile);

	// Scanlines remove light, so the picture needs some of it back or the
	// result just looks dim rather than like a CRT.
	color.rgb = clamp(color.rgb * shade * GetOption(BOOST), 0.0, 1.0);

	SetOutput(color);
}
