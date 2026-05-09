# WorldGenerator

A procedural world generation using Voronoi diagrams for Unreal Engine.

##Map Generation Pipeline:

    Generates random seed points and applies Lloyd's relaxation algorithm to create an evenly distributed Voronoi diagram
    Computes biome properties (height, heat, moisture) using Perlin noise for each Voronoi cell
    Assigns biomes based on height, heat, and moisture values
    Generates resources for each region based on biome type and modifiers

##Mesh Generation:

    Uses UE's GeometryScript to create procedural meshes
    Splits the Voronoi diagram into individual region meshes

##Additional Features:

    Border region detection (water bodies)
    Terrain modifiers (e.g., hills)
    Player spawn region finding
    PCG (Procedural Content Generation) graph integration for additional generation tasks

The generated world consists of themed biomes with procedural resources distributed according to region properties.
