#define pi32 3.14159265359f

#if defined(SIMPLE_SHADER_VS) || defined(SIMPLE_SHADER_PS)
struct ModelBuffer
{
	matrix model;
	float4 color;
	float3 scale;
	float roughness;
	float metalness;
	float vertexDisplacement;
};

ConstantBuffer<ModelBuffer> modelBuffer : register(b1);

#endif

#ifdef SIMPLE_SHADER_VS
struct SceneBuffer
{
	matrix projview;
	float3 lightPos;
	float3 viewPos;
};

ConstantBuffer<SceneBuffer> sceneBuffer : register(b0);
Texture2D<float> heightMap : register(t1);

SamplerState s : register(s1);

struct VertexIn
{
	float3 position : POSITION;
	float3 normal: NORMAL;
	float3 tangent : TANGENT;
	float3 bitangent: BITANGENT;
	float2 uv : UV;
};

struct VertexOut
{
	float3 normal: NORMAL;
	float3 tangent: TANGENT;
	float3 bitangent: BITANGENT;
	float2 uv : UV;
	float3 lightDir : LIGHT_Dir;
	float3 viewDir : VIEW_Dir;
	float4 position: SV_POSITION;
};

VertexOut simpleShaderVS(VertexIn vertexIn, uint id : SV_InstanceID)
{
	VertexOut result;
	float heightOffset = modelBuffer.vertexDisplacement *heightMap.SampleLevel(s, vertexIn.uv, 0);

	float4 worldPosition = mul(modelBuffer.model, float4(modelBuffer.scale * 
		(vertexIn.position + heightOffset * vertexIn.normal), 1.f));
	result.lightDir = sceneBuffer.lightPos - worldPosition.xyz;
	result.viewDir = sceneBuffer.viewPos - worldPosition.xyz;
	result.position = mul(sceneBuffer.projview, worldPosition);
	result.normal = mul(modelBuffer.model, float4(vertexIn.normal / modelBuffer.scale, 0.f)).xyz;
	result.tangent = mul(modelBuffer.model, float4(vertexIn.tangent * modelBuffer.scale, 0.f)).xyz;
	result.bitangent = mul(modelBuffer.model, float4(vertexIn.bitangent * modelBuffer.scale, 0.f)).xyz;
	result.uv = vertexIn.uv;

	return result;
}
#endif


#ifdef SIMPLE_SHADER_PS 
struct PixelIn
{
	float3 normal: NORMAL;
	float3 tangent: TANGENT;
	float3 bitangent: BITANGENT;
	float2 uv : UV;
	float3 lightDir : LIGHT_DIR;
	float3 viewDir : VIEW_DIR;
};

Texture2D<float4> normalMap : register(t0);

SamplerState s : register(s0);

float NDF(float n_dot_h, float roughness)
{
	float roughness2 = roughness * roughness;
	float denom = 1.f + (roughness2 - 1.f) * n_dot_h*n_dot_h;
	return roughness2 / (denom*denom*pi32);
}

float SchlickGGX(float cos_angle, float roughness)
{
	float k = (roughness + 1.f)*(roughness + 1.f) * 0.125f;
	return cos_angle / (cos_angle * (1.f - k) + k);
}

float3 FresnelSchlick(float3 F0, float h_dot_v)
{
	float powTerm = 1.f - h_dot_v;
	powTerm = powTerm * powTerm * powTerm * powTerm * powTerm; //^5
	return F0 + (1.f - F0) * powTerm;
}

float4 simpleShaderPS(PixelIn pixelIn) : SV_Target
{
	//float3x3 TBN = { normalize(pixelIn.tangent), normalize(pixelIn.bitangent), normalize(pixelIn.normal) };
	float3x3 TBN = { (pixelIn.tangent), (pixelIn.bitangent), (pixelIn.normal) };
	TBN = transpose(TBN);

	float3 n = normalMap.Sample(s, pixelIn.uv).xyz;
	n = 2.f * n - 1.f;
	n.xy *= modelBuffer.vertexDisplacement;
	n = normalize(mul(TBN, n));
	float3 l = normalize(pixelIn.lightDir);
	float3 v = normalize(pixelIn.viewDir);
	float3 h = normalize(v + l);

	float dothv = saturate(dot(h, v));
	float dotnv = saturate(dot(n, v));
	float dotnl = saturate(dot(n, l));

	float3 albedo = lerp(modelBuffer.color.xyz, 0.f, modelBuffer.metalness);
	float3 F0 = lerp(0.04f, modelBuffer.color.xyz, modelBuffer.metalness);

	float3 fresnel = FresnelSchlick(F0, dothv);
	float ndf = NDF(dot(n, h), modelBuffer.roughness);
	float geometry = SchlickGGX(dotnl, modelBuffer.roughness) * SchlickGGX(dotnv, modelBuffer.roughness);
	
	float specularDenom = (4.f *dotnl*dotnv + 0.001f);
	float3 specular = fresnel * geometry * ndf / specularDenom;

	float3 lambert = (1.f-fresnel)*albedo / pi32; //TODO: this is not correct!!!

	float intensity = 5.f;
	float ambient = 0.1f;
	
	float3 color = (specular + lambert)*dotnl * intensity + lambert*ambient;


	//float4 color = pixelIn.color;
	//color = color * (saturate(dot(n, l)) * intensity + ambient);


	if (dot(n, normalize(pixelIn.normal)) < 0.1f) //normal vector is very distorted comared to the actual geometry
	{
		color = float3(1.f, 0.f, 0.f);
	}

	//return normalMap.Sample(s, pixelIn.uv);
	return float4(color, 1.f);
}
#endif