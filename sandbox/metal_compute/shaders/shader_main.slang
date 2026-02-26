
import "shader_lib";

RWStructuredBuffer<int> outputBuffer;

[shader("compute")]
[numthreads(4, 1, 1)]
void computeMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int index = (int)dispatchThreadID.x;
    outputBuffer[index] = outputBuffer[index] + foo(index);
}
