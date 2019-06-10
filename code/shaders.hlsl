#define pi32 3.14159265359f

struct ModelBuffer
{
	matrix model;
	float4 color;
	float scale;
	float roughness;
	float metalness;
	float vertexDisplacement;
	float fractalZoomScale;
	int fractalIndex;
	float heightMapFractalZoomScale;
	int heightMapFractalIndex;
	float emissionScale;
};
struct LightBuffer
{
	float3 pos;
	float3 intensity;
};

struct  SceneBuffer
{
	matrix projview;
	float3 viewPos;
	uint lightCount;
	LightBuffer lights[8];
};

struct VertexIn
{
	float3 position : POSITION;
	float3 normal: NORMAL;
	float3 tangent : TANGENT;
	float3 bitangent: BITANGENT;
	float4 shapeOp: SHAPE_OP;
	float2 uv : UV;
};

ConstantBuffer<SceneBuffer> sceneBuffer : register(b0);
ConstantBuffer<ModelBuffer> modelBuffer : register(b1);


#ifdef SIMPLE_SHADER_VS

struct VertexOut
{
	float3 normal: NORMAL;
	float3 tangent: TANGENT;
	float3 bitangent: BITANGENT;
	float2 uv : UV;
	float3 worldPos : WORLD_POS;
	float4 position: SV_POSITION;
};

VertexOut simpleShaderVS(VertexIn vertexIn, uint id : SV_InstanceID)
{
	VertexOut result;

	float4 worldPosition = mul(modelBuffer.model, float4(modelBuffer.scale * 
		(vertexIn.position), 1.f));
	result.worldPos = worldPosition.xyz;
	result.position = mul(sceneBuffer.projview, worldPosition);
	result.normal = mul(modelBuffer.model, float4(vertexIn.normal, 0.f)).xyz;
	result.tangent = mul(modelBuffer.model, float4(vertexIn.tangent * modelBuffer.scale, 0.f)).xyz;
	result.bitangent = mul(modelBuffer.model, float4(vertexIn.bitangent * modelBuffer.scale, 0.f)).xyz;
	result.uv = vertexIn.uv;

	return result;
}
#endif

#ifdef HEIGHT_MAPPING_SHADER_VS

Texture2D<float> heightMaps[2] : register(t2);
SamplerState s : register(s0);

struct VertexOut
{
	float3 normal: NORMAL;
	float3 tangent: TANGENT;
	float3 bitangent: BITANGENT;
	float2 uv : UV;
	float3 worldPos : WORLD_POS;
	float4 shapeOp : SHAPE_OP;
	float4 position: SV_POSITION;
};

VertexOut heightMappingShaderVS(VertexIn vertexIn, uint id : SV_InstanceID)
{
	VertexOut result;
	float h = 0.f;
	if (modelBuffer.vertexDisplacement != 0.f)
	{
		if (modelBuffer.heightMapFractalIndex >= 0)
		{
			float2 fractalUV = vertexIn.uv;
			fractalUV = fractalUV - 0.5f;
			fractalUV = fractalUV * modelBuffer.heightMapFractalZoomScale;
			fractalUV += 0.5f;

			h = heightMaps[modelBuffer.heightMapFractalIndex].SampleLevel(s, fractalUV, 2.f*(modelBuffer.heightMapFractalZoomScale - 0.5f));
		}
		else
		{
			h = heightMaps[0].SampleLevel(s, vertexIn.uv, 0);
		}
		h *= modelBuffer.vertexDisplacement;
	}

	float4 worldPosition = mul(modelBuffer.model, float4(modelBuffer.scale *
		(vertexIn.position + h* vertexIn.normal), 1.f));
	result.worldPos = worldPosition.xyz;
	result.position = mul(sceneBuffer.projview, worldPosition);
	result.normal = mul(modelBuffer.model, float4(vertexIn.normal, 0.f)).xyz;
	result.tangent = mul(modelBuffer.model, float4(vertexIn.tangent * modelBuffer.scale, 0.f)).xyz;
	result.bitangent = mul(modelBuffer.model, float4(vertexIn.bitangent * modelBuffer.scale, 0.f)).xyz;
	result.uv = vertexIn.uv;
	result.shapeOp = vertexIn.shapeOp / modelBuffer.scale; 

	return result;
}
#endif


#ifdef TESS_SHADER

#define CP_COUNT 3

struct HullConstantDataOut
{
	float edges[3] : SV_TessFactor;
	float inside[1] : SV_InsideTessFactor;
};

struct HullIn
{
	float3 position : POSITION;
	float3 normal: NORMAL;
	float3 tangent : TANGENT;
	float3 bitangent: BITANGENT;
	float2 uv : UV;
};

struct DomainOut
{
	float3 normal: NORMAL;
	float3 tangent: TANGENT;
	float3 bitangent: BITANGENT;
	float2 uv : UV;
	float3 lightDir : LIGHT_Dir;
	float3 viewDir : VIEW_Dir;
	float4 position: SV_POSITION;
};

#endif

#ifdef TESS_SHADER_VS
HullIn tessShaderVS(VertexIn vertexIn)
{
	HullIn result;
	result.position = mul(modelBuffer.model, float4(vertexIn.position*modelBuffer.scale, 1.f)).xyz;
	result.normal = mul(modelBuffer.model, float4(vertexIn.normal / modelBuffer.scale, 0.f)).xyz;
	result.tangent = mul(modelBuffer.model, float4(vertexIn.tangent * modelBuffer.scale, 0.f)).xyz;
	result.bitangent = mul(modelBuffer.model, float4(vertexIn.bitangent * modelBuffer.scale, 0.f)).xyz;
	result.uv = vertexIn.uv;
	return result;
}
#endif


#ifdef TESS_SHADER_HS

Texture2D<float> heightMap : register(t1);
SamplerState s : register(s1);

float viewDistFromEdge(float3 a, float3 b, float3 viewPos)
{
	//float t = dot(b - a, viewPos - a) / dot(b-a, b-a);
	//t = saturate(t);
	//float3 distanceVector = (viewPos - a) - t * (b - a);

	return max(0.1f, min(distance(a, viewPos), distance(b, viewPos)));
	//return max(0.1f, length(distanceVector));

}

float3x3 invert(float3x3 m)
{
	float3x3 result = { cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]) };
	result = transpose(result);
	result = mul((1.f / determinant(result)), result);
	return result;
}

float viewDistFromTriangle(float3 a, float3 b, float3 c)
{
	float3x3 fromTriangleBasis = transpose(float3x3(b-a, c-a, cross(b-a, c-a)));
	float3x3 toTriangleBasis = invert(fromTriangleBasis);

	float3 p = mul(toTriangleBasis, sceneBuffer.viewPos - a);
	p.x = saturate(p.x);
	p.y = saturate(p.y);
	p = mul(fromTriangleBasis, p) + a;

	float3 distanceVector = sceneBuffer.viewPos - p;

	return max(0.1f, length(distanceVector));
}

float2 toPixelCoords(float3 pos)
{
	float4 p = mul(sceneBuffer.projview, float4(pos, 1.f));
	p /= p.w;
	p.x *= 1920.f*0.5f;
	p.y *= 1080.f*0.5f;
	return p.xy;
}

HullConstantDataOut tessShaderHSPatchConst(
	InputPatch<HullIn, CP_COUNT> ip,
	uint patchID : SV_PrimitiveID)
{
	HullConstantDataOut result;

	float h0 = heightMap.SampleLevel(s, ip[0].uv, 6);
	float h1 = heightMap.SampleLevel(s, ip[1].uv, 6);
	float h2 = heightMap.SampleLevel(s, ip[2].uv, 6);

	float3 p0 = ip[0].position + ip[0].normal * h0;
	float3 p1 = ip[1].position + ip[1].normal * h1;
	float3 p2 = ip[2].position + ip[2].normal * h2;

	float2 r0 = toPixelCoords(p0);
	float2 r1 = toPixelCoords(p1);
	float2 r2 = toPixelCoords(p2);

	float dist1 = viewDistFromEdge(p2, p0, sceneBuffer.viewPos);
	float dist2 = viewDistFromEdge(p0, p1, sceneBuffer.viewPos);
	float dist0 = viewDistFromEdge(p1, p2, sceneBuffer.viewPos);
	
	float distInside = min(dist1, min(dist0, dist2));// viewDistFromTriangle(p0, p1, p2);
	
	float tessMultiplier = 2000.f;//0.125f * 0.5f;
	
	float e = 1.f;
	result.edges[0] = lerp(64.f, 1.f, pow(saturate(dist0 / 50.f), e));//min(64.f, tessMultiplier / dist0);
	result.edges[1] = lerp(64.f, 1.f, pow(saturate(dist1 / 50.f), e));//min(64.f, tessMultiplier / dist1);
	result.edges[2] = lerp(64.f, 1.f, pow(saturate(dist2 / 50.f), e));//min(64.f, tessMultiplier / dist2);
	result.inside[0] = lerp(64.f, 1.f, pow(saturate(distInside / 50.f), e));//min(64.f, tessMultiplier / distInside);



	return result;
}



[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(CP_COUNT)]
[patchconstantfunc("tessShaderHSPatchConst")]

HullIn tessShaderHS(
	InputPatch<HullIn, CP_COUNT> ip,
	uint cpID : SV_OutputControlPointID,
	uint patchID : SV_PrimitiveID)
{
	HullIn result;

	result = ip[cpID];

	return result;
}
#endif

#ifdef TESS_SHADER_DS

Texture2D<float> heightMap : register(t1);

SamplerState s : register(s1);

float3 baryInt(float3 a, float3 b, float3 c, float3 uvw)
{
	return a * uvw.x + b * uvw.y + c * uvw.z;
}

float2 baryInt(float2 a, float2 b, float2 c, float3 uvw)
{
	return a * uvw.x + b * uvw.y + c * uvw.z;
}


[domain("tri")]
DomainOut tessShaderDS(
	HullConstantDataOut input,
	float3 uvw : SV_DomainLocation,
	OutputPatch<HullIn, CP_COUNT> cps

)
{
	HullIn vertexIn;
	vertexIn.position = baryInt(cps[0].position, cps[1].position, cps[2].position, uvw);
	vertexIn.normal = baryInt(cps[0].normal, cps[1].normal, cps[2].normal, uvw);
	vertexIn.tangent = baryInt(cps[0].tangent, cps[1].tangent, cps[2].tangent, uvw);
	vertexIn.bitangent = baryInt(cps[0].bitangent, cps[1].bitangent, cps[2].bitangent, uvw);
	vertexIn.uv = baryInt(cps[0].uv, cps[1].uv, cps[2].uv, uvw);

	DomainOut result;
	int level = max(0.f, log2(64.f / input.inside[0]));
	float heightOffset = modelBuffer.vertexDisplacement *heightMap.SampleLevel(s, vertexIn.uv, level);

	float4 worldPosition = float4(vertexIn.position, 1.f);
	worldPosition.xyz += vertexIn.normal * heightOffset;
	result.lightDir = sceneBuffer.lightPos - worldPosition.xyz;
	result.viewDir = sceneBuffer.viewPos - worldPosition.xyz;
	result.position = mul(sceneBuffer.projview, worldPosition);
	result.normal = vertexIn.normal;
	result.tangent = vertexIn.tangent;
	result.bitangent = vertexIn.bitangent;
	result.uv = vertexIn.uv;

	return result;
}

#endif


#ifdef FRACTAL_SHADER_PS 
struct PixelIn
{
	float3 normal: NORMAL;
	float3 tangent: TANGENT;
	float3 bitangent: BITANGENT;
	float2 uv : UV;
	float3 lightDir : LIGHT_DIR;
	float3 viewDir : VIEW_DIR;
};

Texture2D<float> fractalImage : register(t0);

SamplerState s : register(s0);

float4 fractalShaderPS(PixelIn pixelIn) : SV_Target
{
	float2 fractalUV = pixelIn.uv;

	fractalUV = fractalUV - 0.5f;
	fractalUV = fractalUV * modelBuffer.zoomScale;
	fractalUV += 0.5f;

	float fractal = 0.9f*fractalImage.SampleLevel(s, fractalUV, 2.f*(modelBuffer.zoomScale - 0.5f)) + 0.3f;
	float3 color = fractal;

	return float4(color, 1.f);
}
#endif

//helper functions for PBR
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
//

#ifdef PBR_SHADER_PS 
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
Texture2D<float4> heightMap : register(t1);

SamplerState s : register(s0);



float4 pbrShaderPS(PixelIn pixelIn) : SV_Target
{
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

	float3 matColor = modelBuffer.color.xyz;
	float3 albedo = lerp(matColor, 0.f, modelBuffer.metalness);
	float3 F0 = lerp(0.04f, matColor, modelBuffer.metalness);

	float3 fresnel = FresnelSchlick(F0, dothv);
	float ndf = NDF(dot(n, h), modelBuffer.roughness);
	float geometry = SchlickGGX(dotnl, modelBuffer.roughness) * SchlickGGX(dotnv, modelBuffer.roughness);

	float specularDenom = (4.f *dotnl*dotnv + 0.001f);
	float3 specular = fresnel * geometry * ndf / specularDenom;

	float3 lambert = (1.f - fresnel)*albedo / pi32; //TODO: this is not correct!!!

	float intensity = 5.f;
	float ambient = 0.1f;

	float3 color = (specular + lambert)*dotnl * intensity + lambert * ambient;

	if (dot(n, normalize(pixelIn.normal)) < 0.01f) //normal vector is very distorted compared to the actual geometry
	{
		color = float3(1.f, 0.f, 0.f);
	}

	return float4(color, 1.f);
}
#endif

#ifdef PBR_NORMAL_CALC_SHADER_PS 
struct PixelIn
{
	float3 normal: NORMAL;
	float3 tangent: TANGENT;
	float3 bitangent: BITANGENT;
	float2 uv : UV;
	float3 worldPos : WORLD_POS;
	float4 shapeOp : SHAPE_OP;
};

Texture2D<float4> normalMap[2] : register(t0);
Texture2D<float> heightMap[2] : register(t2);

Texture2D<float4> fractalMaps[2] : register(t4);

SamplerState s : register(s0);

float4 pbrNormalCalcShaderPS(PixelIn pixelIn) : SV_Target
{

#if 1
	float w11 = pixelIn.shapeOp.x;
	float w12 = pixelIn.shapeOp.y;
	float w21 = pixelIn.shapeOp.z;
	float w22 = pixelIn.shapeOp.w;
#else
	float w11 = 0.f;
	float w12 = 0.f;
	float w21 = 0.f;
	float w22 = 0.f;
#endif
	//calculating normal properly
	float3 n;
	if (modelBuffer.vertexDisplacement == 0.f)
	{
		n = normalize(pixelIn.normal);
	}
	else
	{
		float h;
		float dhdu;
		float dhdv;

		if (modelBuffer.heightMapFractalIndex >= 0)
		{
			float2 fractalUV = pixelIn.uv;
			fractalUV = fractalUV - 0.5f;
			fractalUV = fractalUV * modelBuffer.heightMapFractalZoomScale;
			fractalUV += 0.5f;

			float3 nH = normalMap[modelBuffer.heightMapFractalIndex].SampleLevel(s, fractalUV, 2.f*(modelBuffer.heightMapFractalZoomScale - 0.5f)).xyz;
			nH = 2.f * nH - 1.f;
			nH /= nH.z;
			dhdu = -nH.x * modelBuffer.vertexDisplacement * modelBuffer.heightMapFractalZoomScale;
			dhdv = -nH.y * modelBuffer.vertexDisplacement * modelBuffer.heightMapFractalZoomScale;
			
			h  = heightMap[modelBuffer.heightMapFractalIndex].SampleLevel(s, fractalUV, 2.f*(modelBuffer.heightMapFractalZoomScale - 0.5f));
			h *= modelBuffer.vertexDisplacement;
		}
		else
		{
			float3 nH = normalMap[0].Sample(s, pixelIn.uv).xyz;
			nH = 2.f * nH - 1.f;
			nH = normalize(nH);
			nH /= nH.z;
			dhdu = -nH.x * modelBuffer.vertexDisplacement;
			dhdv = -nH.y * modelBuffer.vertexDisplacement;

			h  = heightMap[0].Sample(s, pixelIn.uv);
			h *= modelBuffer.vertexDisplacement;
		}

		float3x3 TBN = { (pixelIn.tangent), (pixelIn.bitangent), normalize(pixelIn.normal) };
		TBN = transpose(TBN);
	
		float3 t = mul(TBN, float3(1.f, 0.f, dhdu) + h * float3(w11, w12, 0.f));
		float3 b = mul(TBN, float3(0.f, 1.f, dhdv) + h * float3(w21, w22, 0.f));
	
		n = normalize(cross(t, b));
	
		//float3 nS = normalize(pixelIn.normal);
		//float3 dSdu = pixelIn.tangent;
		//float3 dSdv = pixelIn.bitangent;
		//
		//float3 dnSdu = dSdu; //Only for Sphere!!!
		//float3 dnSdv = dSdv; //Only for Sphere!!!
		//
		//
		//float3 dfdu = dSdu + dhdu * nS + h * dnSdu;
		//float3 dfdv = dSdv + dhdv * nS + h * dnSdv;
		//
		//n = -normalize(cross(dfdu, dfdv));
	}	

	float3 matColor = modelBuffer.color.xyz;
	if (modelBuffer.fractalIndex >= 0)
	{
		float2 fractalUV = pixelIn.uv;

		fractalUV = fractalUV - 0.5f;
		fractalUV = fractalUV * modelBuffer.fractalZoomScale;
		fractalUV += 0.5f;

		float4 fractal = fractalMaps[modelBuffer.fractalIndex].SampleLevel(s, fractalUV, 2.f*(modelBuffer.fractalZoomScale - 0.5f));
		if (fractal.w == 1.f) //TODO: use a flag in modeSlBuffer!!!
		{
			fractal.xyz = fractal.x;
		}
		matColor *= fractal.xyz;
	}


	float3 v = normalize(sceneBuffer.viewPos - pixelIn.worldPos);
	float dotnv = saturate(dot(n, v));

	float3 emission = matColor * modelBuffer.emissionScale * max(dotnv, 0.2f);
	matColor = matColor * (1.f - modelBuffer.emissionScale);

	float3 result = emission;
	if (modelBuffer.emissionScale < 1.f)
	{
		float3 albedo = lerp(matColor, 0.f, modelBuffer.metalness);
		float3 F0 = lerp(0.04f, matColor, modelBuffer.metalness);
		float r = modelBuffer.roughness;

		for (uint lightIndex = 0; lightIndex < sceneBuffer.lightCount; ++lightIndex)
		{
			float3 l = sceneBuffer.lights[lightIndex].pos - pixelIn.worldPos;
			float distSq = dot(l, l);
			l /= sqrt(distSq);

			float3 h = normalize(v + l);

			float dothv = saturate(dot(h, v));
			float dotnl = saturate(dot(n, l));
			float dotnh = saturate(dot(n, h));

			float3 fresnel = FresnelSchlick(F0, dothv);
			float ndf = NDF(dotnh, r);
			float geometry = SchlickGGX(dotnl, r) * SchlickGGX(dotnv, r);

			float specularDenom = (4.f *dotnl*dotnv + 0.001f);
			float3 specular = fresnel * geometry * ndf / specularDenom;

			float3 lambert = (1.f - fresnel)*albedo / pi32; //TODO: this is not correct!!!

			float3 intensity = sceneBuffer.lights[lightIndex].intensity / max(1.f, distSq);

			float a = pow(dot(l, pixelIn.tangent), 2)/dot(pixelIn.tangent, pixelIn.tangent);
			float b = pow(dot(l, pixelIn.bitangent), 2)/dot(pixelIn.bitangent, pixelIn.bitangent);
			float curvature = (w11*a + w22 * b) / (a + b);

			float3 color = (specular + lambert)*dotnl *intensity;// *(atan(-0.8f*curvature) + pi32 * 0.5f);//min(1.f, exp(curvature*0.2f));
			result += color;
		}
	}
	//result = w11 * w22;

	//result = 0.5f*n + 0.5f;
	//float2 fractalUV = pixelIn.uv;
	//
	//fractalUV = fractalUV - 0.5f;
	//fractalUV = fractalUV * modelBuffer.heightMapFractalZoomScale;
	//fractalUV += 0.5f;
	//result = normalMap[modelBuffer.heightMapFractalIndex].SampleLevel(s, fractalUV, 2.f*(modelBuffer.heightMapFractalZoomScale - 0.5f));
	return float4(pow(result, 1.f/2.2f), 1.f);
}



#endif

