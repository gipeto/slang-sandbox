struct PixelShaderInput
{
    float4 Color : COLOR;
};

[shader("pixel")]
float4 pixelMain( PixelShaderInput inputColor ) : SV_Target
{
   // Return gamma corrected color.
   return pow( abs( inputColor.Color ), 1.0f / 2.2f );
}
