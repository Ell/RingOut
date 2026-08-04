/*
CRT -- tube geometry, scanlines, aperture grille and vignette.

Four effects that a real CRT produces together, so they are applied together
rather than as separate shaders:

  curvature  the glass bulges, so the image is sampled through a barrel warp
             and anything pushed off the tube reads as bezel, not stretched edge
  scanlines  gaps between the beam's horizontal passes
  mask       the aperture grille -- each output column favours one phosphor
  vignette   corners of the tube receive less beam energy than the centre

Turn CURVE and MASK to 0 and this degrades gracefully into plain scanlines.

[configuration]

[OptionRangeFloat]
GUIName = Curvature
OptionName = CURVE
MinValue = 0.0
MaxValue = 0.30
StepAmount = 0.01
DefaultValue = 0.10

[OptionRangeFloat]
GUIName = Scanline strength
OptionName = SCANLINE
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.30

[OptionRangeFloat]
GUIName = Scanline count
OptionName = LINES
MinValue = 120.0
MaxValue = 1080.0
StepAmount = 30.0
DefaultValue = 480.0

[OptionRangeFloat]
GUIName = Aperture mask strength
OptionName = MASK
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.15

[OptionRangeFloat]
GUIName = Vignette
OptionName = VIGNETTE
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.25

[OptionRangeFloat]
GUIName = Brightness compensation
OptionName = BOOST
MinValue = 1.0
MaxValue = 2.5
StepAmount = 0.05
DefaultValue = 1.30

[/configuration]
*/

void main()
{
	float2 uv = GetCoordinates();

	// --- tube curvature ---------------------------------------------------
	// Offset from centre, pushed outward proportional to distance squared.
	float2 centre = uv - float2(0.5, 0.5);
	float r2 = dot(centre, centre);
	float2 warped = uv + centre * r2 * GetOption(CURVE);

	// Past the edge of the glass there is no picture. Sampling would clamp and
	// smear the border pixels outward, which reads as a stretched frame rather
	// than the edge of a tube.
	if (warped.x < 0.0 || warped.x > 1.0 || warped.y < 0.0 || warped.y > 1.0)
	{
		SetOutput(float4(0.0, 0.0, 0.0, 1.0));
		return;
	}

	float4 color = SampleLocation(warped);

	// --- scanlines --------------------------------------------------------
	// Spaced in source space so density does not change with window size.
	float pos = fract(warped.y * GetOption(LINES));
	float profile = 1.0 - abs(pos - 0.5) * 2.0;
	color.rgb = color.rgb * (1.0 - GetOption(SCANLINE) * (1.0 - profile));

	// --- aperture grille --------------------------------------------------
	// Repeats every three OUTPUT pixels -- this one is deliberately in output
	// space, because it emulates the physical phosphor pitch of the screen
	// rather than anything in the source image.
	float m = GetOption(MASK);
	float column = fract(uv.x * GetWindowResolution().x / 3.0) * 3.0;
	float3 mask;
	if (column < 1.0)
		mask = float3(1.0, 1.0 - m, 1.0 - m);
	else if (column < 2.0)
		mask = float3(1.0 - m, 1.0, 1.0 - m);
	else
		mask = float3(1.0 - m, 1.0 - m, 1.0);
	color.rgb = color.rgb * mask;

	// --- vignette ---------------------------------------------------------
	float vignette = 1.0 - GetOption(VIGNETTE) * r2 * 2.0;
	color.rgb = color.rgb * clamp(vignette, 0.0, 1.0);

	// Scanlines and mask together remove a lot of light; put some back.
	color.rgb = clamp(color.rgb * GetOption(BOOST), 0.0, 1.0);

	SetOutput(color);
}
