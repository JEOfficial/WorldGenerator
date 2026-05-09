// Copyright Julian Ehler. All Rights Reserved.

#include "WorldGeneration/MapGenerator.h"
#include "World/Region.h"
#include "Voronoi/Voronoi.h"
#include "Math/UnrealMathUtility.h"
#include "Components/DynamicMeshComponent.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/SceneUtilityFunctions.h"
#include "GeometryScript/MeshDecompositionFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "Components/BoxComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "World/StrategyMap.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "DrawDebugHelpers.h"

// Sets default values
AMapGenerator::AMapGenerator()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = false;

	bInitializedRandomPoints = false;
	bInitializedVoronoi = false;

	//RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	MapMesh = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("MapMesh"));
	MapGraph = CreateDefaultSubobject<UPCGComponent>(TEXT("MapGraph"));
	PCGVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("PCGVolume"));

	if (MapMesh)
	{
		//MapMesh->SetVisibility(false);
		MapMesh->SetCollisionProfileName(FName("BlockAll"));
		MapMesh->bEnableComplexCollision = true;
		MapMesh->EnableComplexAsSimpleCollision();
	}
	if (MapGraph)
	{
		MapGraph->GenerationTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
	}
	if (PCGVolume)
	{
		PCGVolume->SetupAttachment(RootComponent);
		PCGVolume->SetRelativeLocation(FVector(VORONOIBOX_X * MapScale / 2.f, VORONOIBOX_Y * MapScale / 2.f, 0.f));
		PCGVolume->InitBoxExtent(FVector(VORONOIBOX_X * MapScale / 2.f, VORONOIBOX_Y * MapScale / 2.f, 100.f));
		PCGVolume->SetGenerateOverlapEvents(false);
		PCGVolume->SetCollisionProfileName(TEXT("NoCollision"));
	}

	if (!MapMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("MapMaterial not set on %s, please set in blueprint!"), *GetName());
	}
}

// Called when the game starts or when spawned
void AMapGenerator::BeginPlay()
{
	Super::BeginPlay();

	if (!Biomes)
	{
		UE_LOG(LogTemp, Warning, TEXT("Biomes not set on %s, please set in blueprint!"), *GetName());
	}
}

// --------------------------------------------------------------------------------------------
// VORONOI
// --------------------------------------------------------------------------------------------

void AMapGenerator::GenerateRandomPoints()
{
	// RANDOM POINTS

	for (int32 i = 0; i < VORONOICELLS; i++)
	{
		VoronoiSites.Add(FVector(RandStream.FRandRange(0, VORONOIBOX_X), RandStream.FRandRange(0, VORONOIBOX_Y), 0.f));
	}

	bInitializedRandomPoints = true;
}

void AMapGenerator::VoronoiWithLloyd()
{
	if (bInitializedRandomPoints)
	{
		// LLOYD

		for (int i = 0; i < LLOYD_RELAXATIONS; i++)
		{
			MapVoronoi = new FVoronoiDiagram(VoronoiSites, VORONOICELLS);

			TArray<FVoronoiCellInfo> CellInfos;
			MapVoronoi->ComputeAllCells(CellInfos);

			int32 CellIndex = 0;

			for (const FVoronoiCellInfo& CellInfo : CellInfos)
			{
				VoronoiSites[CellIndex].X = 0;
				VoronoiSites[CellIndex].Y = 0;

				for (const FVector& Vertice : CellInfo.Vertices)
				{
					VoronoiSites[CellIndex].X += Vertice.X;
					VoronoiSites[CellIndex].Y += Vertice.Y;
				}
				VoronoiSites[CellIndex].X /= CellInfo.Vertices.Num();
				VoronoiSites[CellIndex].Y /= CellInfo.Vertices.Num();
				CellIndex++;
			}
		}

		ComputeVoronoiInfo();

		bInitializedVoronoi = true;
	}
}

void AMapGenerator::ComputeVoronoiInfo()
{
	TArray<FVoronoiCellInfo> CellInfos;
	MapVoronoi->ComputeAllCells(CellInfos);

	for(int32 i = 0; i < CellInfos.Num(); ++i)
	{
		RegionGenerationDatas.Add(FRegionGenerationData(VoronoiSites[i], MapScale));

		for (int32 y = 0; y < CellInfos[i].Vertices.Num(); ++y)
		{
			// Set region border.
			if (CellInfos[i].Vertices[y].X <= 0 || CellInfos[i].Vertices[y].X >= VORONOIBOX_X || 
				CellInfos[i].Vertices[y].Y <= 0 || CellInfos[i].Vertices[y].Y >= VORONOIBOX_Y)
			{
				RegionGenerationDatas[i].bIsBorder = true;
				break;
			}
		}
	}
}

// --------------------------------------------------------------------------------------------
// MAP
// --------------------------------------------------------------------------------------------

void AMapGenerator::Generate()
{
	VoronoiSites.Empty();
	RegionGenerationDatas.Empty();
	Regions.Empty();
	MapMesh->GetDynamicMesh()->Reset();

	// Set seed for random generators.
	RandStream = FRandomStream(MapSeed);

	GenerateRandomPoints();

	VoronoiWithLloyd();

	GenerateBiomes();

	DrawBiomeTexture();

	//GenerateMapMesh();
	GenerateRegions();

	//GeneratePCG();
}

void AMapGenerator::GenerateBiomes()
{
	FVector2D RegionPositionNormalized;
	FVector2D RegionPosition;

	float Perlin;
	float PerlinNormalized;

	// Gradient for earth like heat (poles / aquator)
	float GradientX;
	float GradientSin;

	// Fracture for more variant in gradient.
	float Fracture;

	for (int32 i = 0; i < RegionGenerationDatas.Num(); ++i)
	{
		RegionPositionNormalized = FVector2D(2.f * RegionGenerationDatas[i].VoronoiPosition.X / VORONOIBOX_X - 1.f, 2.f * RegionGenerationDatas[i].VoronoiPosition.Y / VORONOIBOX_Y - 1.f);
		RegionPosition = FVector2D(RegionGenerationDatas[i].VoronoiPosition.X, RegionGenerationDatas[i].VoronoiPosition.Y);

		// REGION HEIGHT.
		Perlin = FMath::PerlinNoise2D(RegionPositionNormalized + MapSeed);
		PerlinNormalized = (Perlin + 1.f) / 2.f;

		RegionGenerationDatas[i].Height = PerlinNormalized;

		// REGION HEAT.
		Perlin = FMath::PerlinNoise2D(RegionPosition + MapSeed + 1.f);
		//Perlin = FMath::PerlinNoise2D(FVector2D(RegionGenerationDatas[i]->VoronoiPosition.X, RegionGenerationDatas[i]->VoronoiPosition.Y));
		PerlinNormalized = (Perlin + 1.f) / 2.f;

		// Heat fracture.
		GradientX = (RegionGenerationDatas[i].VoronoiPosition.Y / ((float)VORONOIBOX_Y / PI));
		GradientSin = FMath::Clamp(FMath::Sin((HeatMultiplier * 1.2f) * GradientX - ((((HeatMultiplier * 1.2f) - 1.f) / 2.f) * PI)), 0.f, 1.f);
		Fracture = FMath::Clamp(PerlinNormalized + 0.3f, 0.f, 1.f);
		//Regions[i]->Heat = ((sin * fracture) + Regions[i]->Height) / 2;

		RegionGenerationDatas[i].Heat = GradientSin * Fracture;

		// REGION MOISTURE.
		Perlin = FMath::PerlinNoise2D(RegionPosition + MapSeed + 1.f); // TODO: What is better? +1 or +2? +1 = more evenly, +2 = more variant
		PerlinNormalized = (Perlin + 1.f) / 2.f;

		// Moisture with noise. Apply height. Invert to achiev: higher is drier.
		RegionGenerationDatas[i].Moisture = FMath::Clamp((1 - (PerlinNormalized + RegionGenerationDatas[i].Height) / 2) * MoistureMultiplier, 0.f, 1.f);

		// --------------------------------------------------------------------------------------------
		// Region Modifiers
		// --------------------------------------------------------------------------------------------
		
		// REGION HILLS.
		if (UKismetMathLibrary::RandomBoolFromStream(RandStream))
		{
			RegionGenerationDatas[i].Modifiers.AddTag(HillTag);
		}

		// --------------------------------------------------------------------------------------------
		// Region Biomes
		// --------------------------------------------------------------------------------------------

		if (Biomes)
		{
			if (RegionGenerationDatas[i].bIsBorder)
			{
				RegionGenerationDatas[i].bIsWater = true;
				RegionGenerationDatas[i].Biome = Biomes->Biomes.Last();
				goto continueResources;
			}
			else
			{
				if (RegionGenerationDatas[i].Height <= 0.3f + 0.3f * RegionPositionNormalized.Length() * RegionPositionNormalized.Length())
				{
					RegionGenerationDatas[i].bIsWater = true;
					RegionGenerationDatas[i].Biome = Biomes->Biomes.Last(1);
					goto continueResources;
				}
				else
				{
					for (int32 BiomeIndex = 0; BiomeIndex < Biomes->Biomes.Num(); ++BiomeIndex )
					{
						if (RegionGenerationDatas[i].Height >= Biomes->Biomes[BiomeIndex].MinHeight && RegionGenerationDatas[i].Height <= Biomes->Biomes[BiomeIndex].MaxHeight && RegionGenerationDatas[i].Heat >= Biomes->Biomes[BiomeIndex].MinTemperature && RegionGenerationDatas[i].Heat <= Biomes->Biomes[BiomeIndex].MaxTemperature && RegionGenerationDatas[i].Moisture >= Biomes->Biomes[BiomeIndex].MinMoisture && RegionGenerationDatas[i].Moisture <= Biomes->Biomes[BiomeIndex].MaxMoisture)
						{
							RegionGenerationDatas[i].Biome = Biomes->Biomes[BiomeIndex];
							goto continueResources;
						}
					}
				}
			}

			// --------------------------------------------------------------------------------------------
			// Region Resources
			// --------------------------------------------------------------------------------------------
			continueResources:;

			if (Resources)
			{
				for (int32 ResourceIndex = 0; ResourceIndex < Resources->Resources.Num(); ++ResourceIndex)
				{
					for (const TPair<FGameplayTag, FVector2D>& ResourcePerBiome : Resources->Resources[ResourceIndex].ResourcePerBiomes)
					{
						if (ResourcePerBiome.Key.MatchesTag(RegionGenerationDatas[i].Biome.Tag))
						{
							int32 ResourceAmount = RandStream.FRandRange(ResourcePerBiome.Value.X, ResourcePerBiome.Value.Y);

							for (TPair<FGameplayTag, float> ResourceModifier : Resources->Resources[ResourceIndex].ResourceModifiers)
							{
								if (RegionGenerationDatas[i].Modifiers.HasTag(ResourceModifier.Key))
								{
									ResourceAmount *= ResourceModifier.Value;
								}
							}

							RegionGenerationDatas[i].Resources.Add(Resources->Resources[ResourceIndex].Tag, ResourceAmount);
						}
					}
				}
			}
		}
	}
}

void AMapGenerator::GenerateMapMesh()
{
	/*if (MapMesh)
	{
		// Generate voronoi mesh.
		UGeometryScriptDebug* Debug = nullptr;
		FGeometryScriptVoronoiOptions VoronoiOptions;
		VoronoiOptions.Bounds = FBox(FVector(0.f, 0.f, 0.f), FVector(VORONOIBOX_X, VORONOIBOX_Y, 0.f));
		TArray<FVector2D> VoronoiSites;

		for (const FVector RandomPoint : RandomPoints)
		{
			VoronoiSites.Add(FVector2D(FMath::Clamp(RandomPoint.X, 0.f, VORONOIBOX_X), FMath::Clamp(RandomPoint.Y, 0.f, VORONOIBOX_Y)));
		}

		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendVoronoiDiagram2D(MapMesh->GetDynamicMesh(), FGeometryScriptPrimitiveOptions(),
			GetActorTransform(), VoronoiSites, VoronoiOptions);

		//~ End generate voronoi mesh.

		// Scale voronoi mesh.
		UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh(MapMesh->GetDynamicMesh(), FVector(MapSize, MapSize, 1.f));

		UMaterialInstanceDynamic* DynamicMapMaterial = UMaterialInstanceDynamic::Create(MapMaterial, this);
		DynamicMapMaterial->SetTextureParameterValue(FName("MapTexture"), BiomesTexture);
		MapMesh->SetMaterial(0, DynamicMapMaterial);
	}*/
	if (MapMesh)
	{
		// Generate map mesh.

		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(MapMesh->GetDynamicMesh(), FGeometryScriptPrimitiveOptions(), FTransform(), VORONOIBOX_X, VORONOIBOX_Y);

		//~ End generate map mesh.

		// Scale map mesh.
		UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh(MapMesh->GetDynamicMesh(), FVector(MapScale, MapScale, 1.f));
		UGeometryScriptLibrary_MeshTransformFunctions::TranslateMesh(MapMesh->GetDynamicMesh(), FVector((VORONOIBOX_X * MapScale) / 2, (VORONOIBOX_Y * MapScale) / 2, 1.f));

		// Set map material.
		UMaterialInstanceDynamic* DynamicMapMaterial = UMaterialInstanceDynamic::Create(MapMaterial, this);
		DynamicMapMaterial->SetTextureParameterValue(FName("MapTexture"), BiomesTexture);
		DynamicMapMaterial->SetScalarParameterValue(FName("MapSizeX"), VORONOIBOX_X);
		DynamicMapMaterial->SetScalarParameterValue(FName("MapSizeY"), VORONOIBOX_Y);
		MapMesh->SetMaterial(0, DynamicMapMaterial);
	}
}

void AMapGenerator::GenerateRegions()
{
	if (MapMesh)
	{
		// Generate voronoi mesh.
		UGeometryScriptDebug* Debug = nullptr;
		FGeometryScriptVoronoiOptions VoronoiOptions;
		VoronoiOptions.Bounds = FBox(FVector(0.f, 0.f, 0.f), FVector(VORONOIBOX_X, VORONOIBOX_Y, 0.f));
		TArray<FVector2D> VoronoiSites2D;

		for (const FVector &VoronoiSite : VoronoiSites)
		{
			VoronoiSites2D.Add(FVector2D(FMath::Clamp(VoronoiSite.X, 0.f, VORONOIBOX_X), FMath::Clamp(VoronoiSite.Y, 0.f, VORONOIBOX_Y)));
		}

		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendVoronoiDiagram2D(MapMesh->GetDynamicMesh(), FGeometryScriptPrimitiveOptions(),
			GetActorTransform(), VoronoiSites2D, VoronoiOptions);

		//~ End generate voronoi mesh.

		// Scale voronoi mesh.
		UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh(MapMesh->GetDynamicMesh(), FVector(MapScale, MapScale, 1.f));

		// Set map material.
		UMaterialInstanceDynamic* DynamicMapMaterial = UMaterialInstanceDynamic::Create(MapMaterial, this);
		DynamicMapMaterial->SetTextureParameterValue(FName("MapTexture"), BiomesTexture);
		DynamicMapMaterial->SetScalarParameterValue(FName("MapSizeX"), VORONOIBOX_X);
		DynamicMapMaterial->SetScalarParameterValue(FName("MapSizeY"), VORONOIBOX_Y);
		MapMesh->SetMaterial(0, DynamicMapMaterial);

		// Split voronoi mesh into region meshes.
		TArray<UDynamicMesh*> RegionMeshes;
		TArray<int> ComponentPolygroups;
		UDynamicMeshPool* MeshPool = UGeometryScriptLibrary_SceneUtilityFunctions::CreateDynamicMeshPool();

		UGeometryScriptLibrary_MeshDecompositionFunctions::SplitMeshByPolygroups(MapMesh->GetDynamicMesh(), FGeometryScriptGroupLayer(),
			RegionMeshes, ComponentPolygroups, MeshPool);

		// Spawn region meshes.
		for (int32 i = 0; i < RegionMeshes.Num(); ++i)
		{
			// Scale region mesh uvs
			if (RegionMeshes[i])
			{
				TriangulateRegionMesh(RegionMeshes[i]);
				UGeometryScriptLibrary_MeshUVFunctions::RecomputeMeshUVs(RegionMeshes[i], 0, FGeometryScriptRecomputeUVsOptions(), FGeometryScriptMeshSelection());
				UGeometryScriptLibrary_MeshUVFunctions::ScaleMeshUVs(RegionMeshes[i], 0, FVector2D(0.0001f, 0.0001f), // TODO: Scale with map size.
					FVector2D(0, 0), FGeometryScriptMeshSelection());
			}

			if (RegionClass)
			{
				// Create region actors.
				ARegion* RegionActor = GetWorld()->SpawnActor<ARegion>(RegionClass);

				if (RegionActor)
				{
					// Set mesh for region actor to region mesh.
					RegionActor->GetDynamicMeshComponent()->SetDynamicMesh(RegionMeshes[i]);

					// Get mesh position to get the correct voronoi cell and set as region position.
					FVoronoiComputeHelper Helper = MapVoronoi->GetComputeHelper();
					FVector MeshTranslation = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(RegionMeshes[i]).GetCenter();
					//UGeometryScriptLibrary_MeshTransformFunctions::TranslateMesh(RegionMeshes[i], -MeshTranslation);
					int32 RegionID = MapVoronoi->FindCell(FVector(MeshTranslation.X / MapScale, MeshTranslation.Y / MapScale, 0), Helper);

					float SurfaceArea, Volume;
					UGeometryScriptLibrary_MeshQueryFunctions::GetMeshVolumeArea(RegionMeshes[i], SurfaceArea, Volume); // TODO: Implement volume or delete.

					// Init region.

					// Set Region Material.
					RegionActor->GetDynamicMeshComponent()->SetMaterial(0, RegionGenerationDatas[RegionID].Biome.BiomeMaterial);

					RegionActor->InitRegion(&RegionGenerationDatas[RegionID]);
					
					Regions.Add(RegionActor);
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("RegionClass not set on %s, please set in blueprint!"), *GetName()); // TODO: Create own log channel for map generator.
			}
		}
	}
}

void AMapGenerator::TriangulateRegionMesh(UDynamicMesh* TargetMesh)
{
	if (!TargetMesh)
	{
		return;
	}

	// Use EditMesh to safely get a reference to the mesh for writing
	TargetMesh->EditMesh([&](FDynamicMesh3& Mesh)
		{
			// Get all vertices from the mesh
			TArray<FVector3d> UnsortedVertices;
			for (int VID : Mesh.VertexIndicesItr())
			{
				UnsortedVertices.Add(Mesh.GetVertex(VID));
			}

			if (UnsortedVertices.Num() < 3)
			{
				return; // Not enough vertices to triangulate
			}

			// Calculate the center point
			FVector3d Center = FVector3d::Zero();
			for (const FVector3d& Vertex : UnsortedVertices)
			{
				Center += Vertex;
			}
			Center /= UnsortedVertices.Num();
			//DrawDebugPoint(
			//	GetWorld(), // The UWorld context
			//	Center,                 // The position to draw the point
			//	10.0f,                  // Size of the point
			//	FColor::Green,            // Color of the point
			//	true,                   // Persistent: true to keep it visible
			//	-1.0f                   // Lifetime: -1.0f means infinite lifetime
			//);

			// Sort the vertices by their angle around the center point
			UnsortedVertices.Sort([&](const FVector3d& A, const FVector3d& B)
				{
					// Calculate vectors from the center to the vertices
					FVector3d V1 = A - Center;
					FVector3d V2 = B - Center;

					// Get the angle in a consistent plane (e.g., XY plane)
					float Angle1 = FMath::Atan2(V1.Y, V1.X);
					float Angle2 = FMath::Atan2(V2.Y, V2.X);

					return Angle1 > Angle2;
				});

			// Now, the vertices are ordered correctly
			TArray<FVector3d> PolygonVertices = UnsortedVertices;

			// Clear the mesh to prepare for new geometry
			Mesh.Clear();

			// Add the center point and polygon vertices
			int32 CenterVID = Mesh.AppendVertex(Center);
			TArray<int32> VertexIDs;
			for (const FVector3d& Vertex : PolygonVertices)
			{
				//UE_LOG(LogTemp, Warning, TEXT("Vert: %f, %f, %f"), Vertex.X, Vertex.Y, Vertex.Z);
				//DrawDebugPoint(
				//	GetWorld(), // The UWorld context
				//	Vertex,                 // The position to draw the point
				//	10.0f,                  // Size of the point
				//	FColor::Red,            // Color of the point
				//	true,                   // Persistent: true to keep it visible
				//	-1.0f                   // Lifetime: -1.0f means infinite lifetime
				//);
				VertexIDs.Add(Mesh.AppendVertex(Vertex));
			}

			// Perform the triangulation
			for (int i = 0; i < VertexIDs.Num(); ++i)
			{
				int32 V1ID = VertexIDs[i];
				int32 V2ID = VertexIDs[(i + 1) % VertexIDs.Num()];

				Mesh.AppendTriangle(V1ID, V2ID, CenterVID);
			}
		});
}

AStrategyMap* AMapGenerator::FinishGeneration()
{
	AStrategyMap* StrategyMap = GetWorld()->SpawnActor<AStrategyMap>();
	StrategyMap->Initialize(MapVoronoi, Regions, FVector2D(VORONOIBOX_X, VORONOIBOX_Y), MapScale);

	Destroy();
	return StrategyMap;
}

void AMapGenerator::DrawBiomeTexture()
{
	if (bInitializedVoronoi)
	{
		// START WRITE TEXTURE

		BiomesTexture = UTexture2D::CreateTransient(VORONOIBOX_X, VORONOIBOX_Y);
		BiomesTexture->SRGB = 0; // Disable srgb to only use rgb values. Otherwise comparing the colors in material graph doesnt work.
		BiomesTexture->LODGroup = TextureGroup::TEXTUREGROUP_Pixels2D; // Set texture group to 2D to disable blurring between colors. Blur is useful for borders.
		FTexture2DMipMap* MipMap = &BiomesTexture->GetPlatformData()->Mips[0];
		FByteBulkData* ImageData = &MipMap->BulkData;
		uint8* RawImageData = (uint8*)ImageData->Lock(LOCK_READ_WRITE);

		// DRAW VERTICES

		int32 index = 0;
		for (int32 y = 0; y < VORONOIBOX_Y; y++)
		{
			for (int32 x = 0; x < VORONOIBOX_X; x++)
			{
				FVector CellVector;
				FVoronoiComputeHelper Helper = MapVoronoi->GetComputeHelper();
				int32 CellNumber = MapVoronoi->FindCell(FVector(x, y, 0.f), Helper, CellVector);
				
				RawImageData[index * 4] = RegionGenerationDatas[CellNumber].Biome.ColorB;
				RawImageData[index * 4 + 1] = RegionGenerationDatas[CellNumber].Biome.ColorG;
				RawImageData[index * 4 + 2] = RegionGenerationDatas[CellNumber].Biome.ColorR;
				RawImageData[index * 4 + 3] = 255;

				index++;
			}
		}

		// END WRITE TEXTURE

		ImageData->Unlock();
		BiomesTexture->UpdateResource();
	}
}

void AMapGenerator::FindSpawnRegionsForPlayers(TMap<APlayerController*, int32>& PlayerSpawnRegions)
{
	for (auto& PlayerSpawnRegion : PlayerSpawnRegions)
	{
		for (int32 i = 0; i < RegionGenerationDatas.Num(); ++i)
		{
			if (!RegionGenerationDatas[i].bIsWater && RegionGenerationDatas[i].Biome.Tag == FGameplayTag::RequestGameplayTag("Biome.Desert"))
			{
				UE_LOG(LogTemp, Warning, TEXT("%f, %f"), RegionGenerationDatas[i].VoronoiPosition.X, RegionGenerationDatas[i].VoronoiPosition.Y);
				PlayerSpawnRegion.Value = i;
				return;
			}
		}
	}
}

// --------------------------------------------------------------------------------------------
// PCG
// --------------------------------------------------------------------------------------------

void AMapGenerator::GeneratePCG()
{
	if (MapGraph && MapGraph->GetGraph())
	{
		Cast<UPCGGraphInstance>(MapGraph->GetGraphInstance()->GetGraphParameter<UObject*>(FName("BiomeTextureProjectionInstance")).GetValue())->SetGraphParameter<UObject*>(FName("TextureToProjectOn"), BiomesTexture);
		Cast<UPCGGraphInstance>(MapGraph->GetGraphInstance()->GetGraphParameter<UObject*>(FName("BiomeTextureProjectionInstance")).GetValue())->SetGraphParameter<FVector2D>(FName("TextureSize"), FVector2D(VORONOIBOX_X * MapScale, VORONOIBOX_Y * MapScale));
		Cast<UPCGGraphInstance>(MapGraph->GetGraphInstance()->GetGraphParameter<UObject*>(FName("BiomeTextureProjectionInstance")).GetValue())->SetGraphParameter<FString>(FName("ChannelsToProject"), FString("xy"));

		MapGraph->Generate();
		//UPCGGraphInstance* TextureGraph = Cast<UPCGGraphInstance>(
		//	MapGraph->GetGraphInstance()->GetGraphParameter<UObject*>(FName("BiomeTextureProjectionInstance")).GetValue());
		//
		//if (TextureGraph)
		//{
		//	TextureGraph->SetGraphParameter<UObject*>(FName("TextureToProjectOn"), MapTexture);
		//
		//	if (MapGraph)
		//	{
		//		MapGraph->Generate();
		//	}
		//}
	}
}
